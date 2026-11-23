# Protocolo de eventos — CLI e GUI

Especificação do canal compartilhado entre a CLI, a GUI e o daemon de sessão da Lia (seção 4.1 do [PLANO.md](../PLANO.md)). A GUI é uma camada fina sobre este mesmo protocolo: nenhuma lógica de execução, autorização ou auditoria é duplicada nela.

> Status: especificação para a fase **0.6**. Agora existem os dois lados:
> `gui/protocolo.c` (cliente puro, usado pela GUI — ver [GUI-POC.md](GUI-POC.md))
> e `sessiond/sessiond.c` (**lia-sessiond**, o daemon — servidor de protocolo
> e máquina de estados). **O executor real do daemon está em produção**
> (`sessiond/executor_real.c`, oito ferramentas locais, uma ferramenta web e duas ações de persistência — ver seção 8);
> o mock (`sessiond/manifesto_mock.c`) existe só para teste. Esta etapa
> também adiciona a ponte com o modelo local (Ollama) para chat — seção 9
> — mantendo o agente inteiramente somente leitura: o modelo nunca aprova
> nem executa nada por si só. A CLI (`lia tools`, `lia shell`, `lia sudo`,
> `lia root`) continua funcionando como processos de execução única, sem o
> socket (seção 6).

## 1. Transporte

- Socket Unix local em **`~/.lia/run/session.sock`** (caminho atualizado nesta etapa — uma versão anterior deste documento registrava `~/.lia/maquina/estado/sessao.sock`; `~/.lia/run/` é um diretório próprio para o runtime da sessão, não para estado persistente). `lia-sessiond` cria `~/.lia/run/` com modo `0700` e o arquivo de socket com `0600`.
- `lia-sessiond` se recusa a iniciar como root (`docs/THREAT-MODEL.md` §6) e remove, na inicialização, um socket "stale" (arquivo de socket sem processo escutando) só depois de confirmar que ele pertence ao mesmo uid e é de fato um socket — nunca remove um arquivo desconhecido nem um socket onde outro daemon já está escutando de verdade.
- Sem rede, sem TCP por padrão. Um transporte remoto (rede Lia, fase 10) é uma extensão explícita e separada deste protocolo, nunca o mesmo socket.
- Autenticação do cliente por credencial de socket Unix (`SO_PEERCRED`): a GUI e a CLI só conversam com um daemon que roda com o mesmo uid, ou um uid explicitamente autorizado pela pessoa.
- Enquadramento: uma mensagem JSON por linha (NDJSON), UTF-8, sem aninhamento de frames binários nesta versão.

## 2. Envelope

Toda mensagem, em qualquer direção, tem este envelope:

```json
{
  "protocolo": "lia.v1",
  "tipo": "...",
  "id": "01J...",
  "em_resposta_a": null,
  "ts": "2026-11-07T10:00:00Z",
  "dados": {}
}
```

- `id`: identificador único da mensagem (ULID/UUID), gerado por quem envia.
- `em_resposta_a`: `id` da mensagem que esta responde ou continua; `null` para o primeiro evento de uma sequência.
- `ts`: data/hora em UTC, formato RFC 3339.
- `dados`: payload específico de `tipo`, descrito abaixo.

Uma versão incompatível do protocolo é rejeitada explicitamente (`erro` com `codigo: "protocolo_incompativel"`), nunca interpretada de forma aproximada.

## 3. Tipos de mensagem

### Cliente → daemon

| `tipo` | `dados` | Efeito |
|---|---|---|
| `solicitar` | `{ "ferramenta": "tools.discover", "parametros": {...}, "modo": "simular" \| "executar", "objetivo"?, "justificativa"?, "origem_modelo"?: bool, "pergunta_id"? }` | Pede a execução (ou simulação) de uma ferramenta do manifesto. Os quatro últimos campos são opcionais e só informativos — ver seção 9: uma pessoa pode copiá-los de uma `proposta` do modelo, mas a validação é idêntica com ou sem eles. |
| `aprovar` | `{ "plano_id": "...", "decisao": "aprovar" \| "rejeitar", "escopo": "uma_vez" \| "sessao" }` | Resposta humana a um `plano` pendente. |
| `cancelar` | `{ "execucao_id": "..." }` | Pede cancelamento cooperativo de uma execução em andamento. |
| `assinar` | `{ "canais": ["auditoria", "execucao"] }` | Assina um fluxo de eventos (ex.: a GUI assina `auditoria` para mostrar o histórico ao vivo). |
| `perguntar` | `{ "mensagem": "...", "contexto"?: "...", "arquivos"?: [{"caminho","conteudo"}] }` | Fala com o modelo (seção 9) — nunca aprova nem executa nada por si só. `contexto` é histórico opcional, não confiável e serve apenas para resolver referências; `mensagem` continua sendo o pedido atual. |

### Daemon → cliente

| `tipo` | `dados` | Significado |
|---|---|---|
| `plano` | `{ "plano_id", "versao_manifesto", "objetivo", "justificativa", "ferramenta", "parametros_normalizados", "passos": [{"ferramenta","parametros"}], "capacidades", "risco", "efeitos", "idempotente": bool, "timeout_padrao_ms", "reversivel", "requer_aprovacao": bool }` | O executor valida o manifesto e descreve o que faria, **antes** de qualquer efeito. `passos` é sempre um único elemento nesta etapa (ver seção 9.3) — existe para o formato já suportar múltiplos passos sem quebrar quem lê `ferramenta`/`parametros_normalizados` hoje. |
| `progresso` | `{ "execucao_id", "fracao": 0.0–1.0, "mensagem": "..." }` | Atualização opcional durante uma execução longa. |
| `saida` | `{ "execucao_id", "fluxo": "stdout" \| "stderr", "trecho": "..." }` | Saída incremental de uma ferramenta que produz texto. |
| `resultado` | `{ "execucao_id", "estado": "sucesso" \| "falha" \| "cancelado", "codigo_saida": int, "dados": {...} }` | Resultado final, sempre emitido mesmo em erro ou cancelamento. |
| `auditoria` | `{ "entrada_id", "ts", "identidade", "ferramenta", "parametros_normalizados", "capacidades", "resultado_resumo", "reversao_disponivel": bool }` | Eco do registro gravado em `~/.lia/humano/auditoria/`; é sempre gravado antes de ser emitido. |
| `erro` | `{ "codigo", "mensagem", "detalhe" }` | Falha do protocolo ou do daemon (não confundir com `resultado.estado = "falha"`, que é falha da ferramenta). |
| `proposta` | `{ "pergunta_id", "resposta_texto", "ferramenta_proposta": {"ferramenta","parametros","justificativa"} \| null }` | Resposta do modelo a `perguntar` (seção 9). Nunca é um plano — só texto e, no máximo, uma sugestão de ferramenta que a pessoa pode decidir encaminhar como `solicitar`. |
| `resposta` | `{ "pergunta_id", "resposta_texto" }` | Resumo do modelo depois de um `resultado` de execução que se originou de uma `proposta` sua (`origem_modelo` em `solicitar`) — nunca propõe uma nova ferramenta. |

## 4. Ciclo de vida de uma ação com efeito

```
cliente                     daemon
   | solicitar(executar)  →   |
   |                       plano (risco, capacidades, requer_aprovacao)
   |  ←  plano               |
   | aprovar/rejeitar      →  |
   |                       (se aprovado) executa via executor de capacidades
   |  ←  progresso* saida*   |
   |  ←  resultado            |
   |  ←  auditoria            |
```

- `requer_aprovacao = false` só é permitido para ferramentas classificadas `leitura` sem efeito (seção 5). Qualquer outro risco exige o passo `aprovar`, mesmo que uma política do usuário responda automaticamente — a política fica registrada como a autorização usada, nunca como ausência de autorização.
- `resultado` é emitido mesmo se o cliente desconectar antes: a execução não fica "solta" sem fechamento.
- `auditoria` é sempre emitido depois de gravado em disco, nunca antes.

## 5. Erros

Códigos estáveis, para a GUI e a CLI tratarem sem regex em mensagem humana:

| `codigo` | Quando |
|---|---|
| `protocolo_incompativel` | Versão de `protocolo` não suportada. |
| `ferramenta_desconhecida` | `ferramenta` não está no manifesto carregado. |
| `parametro_invalido` | Falha de validação de tipo/limite do manifesto. |
| `capacidade_negada` | A política do usuário não concede a capacidade exigida. |
| `aprovacao_pendente` | Uma ação com efeito foi solicitada sem passar por `aprovar`. |
| `cancelado` | Execução interrompida por `cancelar`. |
| `limite_excedido` | Timeout, limite de saída ou de recursos do manifesto foi atingido, **ou** a linha NDJSON recebida passou do tamanho máximo (`lia-sessiond`: 65536 bytes) sem quebra de linha — a conexão é encerrada neste caso, o framing não é mais confiável. |
| `mensagem_invalida` | JSON malformado ou envelope sem `protocolo`/`tipo`/`id`/`ts`, ou `tipo` fora da tabela da seção 3. A conexão é encerrada (ver nota abaixo). |
| `id_duplicado` | `id` já usado antes na mesma conexão. Recuperável: a conexão continua, a mensagem é ignorada. |
| `estado_invalido` | Mensagem válida, mas fora de ordem para o estado atual da conexão (ex.: `aprovar` sem `plano` pendente, `cancelar` sem nada em andamento, `solicitar` com outra solicitação já em andamento, ou um tipo que só o daemon deveria enviar chegando do cliente). Recuperável. |
| `nao_autorizado` | `SO_PEERCRED` do cliente não corresponde ao uid do daemon (seção 7). A conexão é encerrada imediatamente, antes de processar qualquer mensagem. |

**Erros que encerram a conexão** (`protocolo_incompativel`, `mensagem_invalida`, `limite_excedido` por linha longa, `nao_autorizado`): o framing ou a confiança no cliente já estão comprometidos, então `lia-sessiond` não tenta continuar servindo a mesma conexão. Os demais códigos são recuperáveis — a conexão continua e o cliente pode tentar de novo.

## 6. Relação com a CLI de hoje

Os comandos já implementados (`lia tools discover`, `lia tools list [--json]`, `lia tools probe --safe`, `lia shell`, `lia sudo`, `lia root`) continuam funcionando como processos de execução única, sem o socket. Quando o daemon existir, a CLI passa a ser um cliente fino deste protocolo (como a GUI), mantendo os mesmos nomes de ferramenta do manifesto (seção [manifesto.md](manifesto.md)) e a mesma saída `--json` como payload de `resultado.dados`.

## 7. Autenticação do cliente — o que `SO_PEERCRED` prova e o que não prova

`lia-sessiond` usa `getsockopt(SO_PEERCRED)` em toda conexão aceita para ler o uid real do processo do outro lado do socket, e fecha a conexão (com `erro.codigo = "nao_autorizado"`, antes de processar qualquer mensagem) se esse uid não for exatamente o uid efetivo do próprio daemon. Isto é a única verificação de identidade que existe hoje.

**O que isto prova:** que o processo do outro lado do socket roda com o mesmo uid do Linux que o daemon — útil para impedir que outro usuário do mesmo sistema (multiusuário) converse com a sessão.

**O que isto NÃO prova, e nunca deve ser tratado como prova:**
- Não autoriza nenhuma ferramenta nem capacidade — isso continua inteiramente a cargo do executor, contra o manifesto e a política (`docs/manifesto.md` §5), em cada `solicitar`/`aprovar`, sempre.
- Não protege contra outro processo do **mesmo** uid (qualquer programa que a própria pessoa execute nesta sessão de usuário já tem o mesmo uid e passaria esta checagem) — a mitigação real contra isso é a permissão `0600` do arquivo de socket mais o próprio modelo de confiança do sistema operacional para processos do mesmo usuário; `SO_PEERCRED` é uma camada adicional, não a única.
- Se `SO_PEERCRED` não estiver disponível no kernel/plataforma, `lia-sessiond` recusa a conexão por padrão (falha segura — nunca assume identidade sem conseguir verificar).
- Esta checagem é específica de Linux (`SO_PEERCRED`); um port para outro sistema (BSD tem `LOCAL_PEERCRED`, por exemplo) precisa de uma implementação equivalente antes de valer a mesma garantia — não é portável por si só.

## 8. Executor: real por padrão, mock só para teste

O executor é plugável (`sessiond/executor.h`) e dois backends existem:

- **`real`** (`sessiond/executor_real.c`/`sessiond/manifesto_real.c`, padrão em produção) — chama `cli/sistema.c` e `cli/ferramentas.c` de verdade, com oito ferramentas locais, `web.pesquisar`, `web.abrir` (HTTPS externo, risco baixo, aprovação obrigatória) e duas ações explícitas de persistência. `web.pesquisar` retorna fontes e, para pares de moedas conhecidos, valor/data estruturados; `web.abrir` aceita páginas HTTPS públicas, extrai texto limitado, não executa JavaScript e recusa redes privadas e redirecionamentos não validados. As ferramentas locais não gravam nada; `tools.discover.persistir`/`tools.probe.persistir` continuam exigindo aprovação. A única execução de processo externo em todo o backend é `tools.probe`/`tools.probe.persistir` com `--safe`, pelo catálogo fixo já existente em `cli/ferramentas.c` (sandboxed, com timeout, sem shell).
- **`mock`** (`sessiond/executor_mock.c`/`sessiond/manifesto_mock.c`, só via `LIA_SESSIOND_EXECUTOR=mock`) — tabela fixa e fictícia (`mock.leitura`, `mock.escrita` risco médio, `mock.admin` risco admin) só para testar a máquina de estados inteira (plano → aprovação → progresso/saída → resultado → auditoria, replay, cancelamento) sem nenhum efeito real em nenhum caso — nem `mock.admin` jamais chama `sudo`/`pkexec`. **Nunca é o executor padrão em produção** (`sessiond.c`, `escolher_executor`).

Validação de parâmetros (`validar_parametros`) e a política "só capacidades de leitura" (`Executor.restringe_a_leitura`, verificada em `tratar_solicitar` antes de qualquer execução) são do daemon, nunca da GUI — a GUI só reflete o que o daemon decidiu.

`sessiond/manifesto_mock.c` também tem duas ferramentas fictícias (nunca no backend `real`) usadas só para fechar lacunas de teste que o executor real não exercita de forma rápida e determinística: `mock.timeout` (nunca conclui por si só — só o watchdog de timeout de verdade do daemon resolve) e `mock.saida_grande` (reporta `limite_excedido` deterministicamente, sem gerar nenhum dado).

## 9. Chat com o modelo local (Ollama) — sempre somente leitura

Ver `sessiond/modelo.h`, `sessiond/prompt.h` e `docs/THREAT-MODEL.md` §5.2 para o desenho completo. Resumo do que este protocolo garante:

- **O modelo nunca recebe o socket.** A camada de modelo (`ModeloBackend`) não importa nem tem acesso a nenhum símbolo de aprovação/execução — arquiteturalmente, não só por política.
- **O modelo nunca aprova nem executa nada.** `perguntar` produz, no máximo, texto mais uma sugestão de ferramenta (`proposta`). Para essa sugestão ter efeito, uma pessoa precisa encaminhá-la como um `solicitar` normal — que passa pela mesma validação independente (manifesto, capacidades, parâmetros) que qualquer `solicitar` digitado à mão, mesmo marcado com `origem_modelo: true`. Uma ferramenta desconhecida ou proibida proposta pelo modelo é rejeitada exatamente como seria se um humano a tivesse digitado.
- **O modelo nunca altera um plano depois de aprovado.** Ele não escreve em nenhum dos dois lados dessa transação — só a pessoa, via `solicitar`/`aprovar`.
- **Conteúdo de arquivo é sempre dado, nunca instrução.** `perguntar.dados.arquivos[].conteudo` entra no prompt dentro de um bloco delimitado e explicitamente marcado como não confiável (`sessiond/prompt.c`); as instruções de sistema dizem ao modelo para nunca tratar esse conteúdo como comando, mesmo que pareça um.
- **Backend real (Ollama) só fala com `127.0.0.1`/`localhost`.** Endpoint fora disso é rejeitado antes de qualquer E/S (`ollama/cliente_ollama.c`).

Fluxo típico:

```
cliente                     daemon                      modelo
   | perguntar             →  |                            |
   |                          | monta prompt (prompt.c)  →  |
   |                          |                    proposta  |
   |  ←  proposta             |  ←                           |
   | solicitar(origem_modelo) →  |                           |
   |                       plano (validação igual à de sempre)|
   |  ←  plano, aprovar, resultado — fluxo normal da seção 4  |
   |                          | (se origem_modelo) monta novo
   |                          | prompt com o resultado    →  |
   |                          |                     resposta  |
   |  ←  resposta             |  ←                           |
```

`escolher_modelo` (`sessiond.c`) seleciona o backend via `LIA_SESSIOND_MODELO`: **`mock` é o padrão** (determinístico, sem rede — ao contrário do executor, onde o padrão é o `real`, porque aqui `ollama` exige um servidor Ollama de verdade rodando em `127.0.0.1:11434` por padrão). `LIA_SESSIOND_MODELO=ollama` ativa o backend real; `LIA_OLLAMA_HOST`/`LIA_OLLAMA_PORTA`/`LIA_OLLAMA_MODELO`/`LIA_OLLAMA_TIMEOUT_MS` configuram a conexão.

Cliente de referência: `lia-chat` (`chat/lia-chat.c`, `make chat`) — primeiro cliente de chat de terminal deste fluxo, documentado em `docs/LIA-CLI.md` §"`lia-chat`". Implementa exatamente o fluxo acima (`perguntar`→`proposta`→`solicitar`→`plano`→`aprovar`→`resultado`→`resposta`), com aprovação explícita (padrão: rejeitar) e anexação de arquivo (`/arquivo adicionar`, sempre marcado não confiável).

## 10. Fluxo agente: da pergunta à resposta final fundamentada

A seção 9 descreve o que o modelo **não** pode fazer. Esta descreve o caminho completo de uma pergunta de leitura, que não para em "ferramenta sugerida":

```
pergunta → intenção → proposta validada → plano → execução → resultado real → resposta final
```

Cada etapa é uma mensagem (ou um campo) separada — texto do modelo, intenção, proposta, plano, resultado da ferramenta e resposta final nunca se misturam:

| Etapa | Mensagem | O que traz |
|---|---|---|
| intenção + proposta | `proposta` | `resposta_texto` (texto do modelo, ou a mensagem do validador quando a proposta foi removida), `tipo_resposta` (`proposta`/`resposta`/`esclarecimento`), `intencao` (`id`, `descricao`, `confianca`), `ferramenta_proposta` (a **única** coisa que um cliente pode encaminhar como `solicitar`; `null` quando rejeitada) e `validacao` |
| validação | `proposta.dados.validacao` | `origem` (`roteador`, `modelo`, `modelo+roteador`), `veredito` (`rota_direta`, `aceita`, `corrigida`, `sem_ferramenta`, `esclarecer`, `rejeitada`), `motivo`, `ferramenta_do_modelo`, `saida_estruturada`, `tentativas`, `risco`, `requer_aprovacao`, `somente_leitura`, `execucao_automatica_permitida` |
| plano | `plano` | igual à seção 4 — o daemon valida de novo manifesto, capacidades e parâmetros |
| resultado | `resultado` | `dados` (JSON bruto da ferramenta, intacto), `apresentacao` (texto legível derivado **só** desse JSON por `sessiond/apresentacao.c`) e `tempo_execucao_ms` |
| resposta final | `resposta` | `resposta_texto`, `origem` (`deterministica`, `modelo_extrativa`, `modelo`; para pesquisa web também `modelo_sintese` e `extrativa` — §11.8), `fundamentada`, `aviso`, `tempo_modelo_ms` |

**Resposta estruturada do modelo.** Quando o backend suporta (Ollama `format`, llama-server `json_schema`), o daemon pede a saída restrita a um esquema JSON — `{"tipo","ferramenta","parametros","justificativa","resposta"}`, com `ferramenta` como enum dos nomes do manifesto ativo mais `"nenhuma"` — em vez de procurar JSON no meio de texto livre (`modelo_preencher_proposta_estruturada`; o caminho antigo de JSON embutido continua só como compatibilidade). Resposta inválida: nada é executado; há **uma** nova tentativa com o motivo da rejeição; se falhar, a proposta sai sem ferramenta com uma mensagem clara. Nenhuma heurística fabrica parâmetros.

**Piloto automático — só leitura.** `validacao.execucao_automatica_permitida` é calculado pelo **daemon**: verdadeiro apenas para ferramenta declarada `somente_leitura`, com `risco: nenhum`, sem `requer_aprovacao` e só com capacidades de leitura. O cliente só obedece:

- `LIA_CHAT_AUTOPILOT=read-only` (padrão do `lia-chat`): proposta com `execucao_automatica_permitida` é encaminhada como `solicitar` sem perguntar — o mesmo `solicitar` de `/plano`; quem valida e executa continua sendo o daemon, pelo executor real. O cliente nunca chama executor nenhum.
- `LIA_CHAT_AUTOPILOT=manual`: toda ferramenta espera `/plano`.
- qualquer outro valor: `lia-chat` recusa iniciar (código 2). Não existe valor que libere escrita, administração ou hardware.
- Ferramentas que gravam (`tools.*.persistir`), que executam binários (`tools.probe`), administrativas ou de hardware **nunca** seguem sozinhas; se solicitadas, o `plano` para em `aguardando_aprovacao` como sempre. O modelo nunca aprova nada.

**Resposta final.** Depois do `resultado` de uma execução com `origem_modelo`, o daemon sempre fecha o turno com uma `resposta`. Como ela é produzida depende da ferramenta (`apresentacao_modo_resposta`):

- **determinística** (rankings e listas: `sistema.processos_top_cpu`, `sistema.processos_top_memoria`, `sistema.dispositivos`): frase montada pelo daemon a partir do JSON; o modelo não é consultado.
- **extrativa** (`sistema.recursos`, `sistema.diagnosticar`): o modelo recebe as linhas numeradas e devolve só `{"linhas":[...]}`; o texto final é a cópia das linhas escolhidas, unidas às que casam com o que foi perguntado. O modelo não escreve nenhum valor. Seleção ausente/ inválida: ficam as linhas que casam com a pergunta (ou todas), com `aviso`.
- **resumo conferido** (demais ferramentas): texto livre do modelo, aceito só se todo número citado existir nos dados, se não negar os dados e se respeitar a ordem do ranking; senão, resposta determinística com `aviso` dizendo o motivo.

O resumo livre foi restringido assim porque, em teste real com um modelo local de 3B, resumos que passavam na checagem numérica ainda afirmavam coisas falsas ("todos os dispositivos PCI são Intel"; RAM total citada como "disponível").

Execução que falha ou estoura o tempo também termina com `resposta` determinística dizendo que nenhum dado foi coletado.

**Cancelar a conversa com o modelo.** `cancelar` com `dados: {"pergunta": true}` (ou um `cancelar` simples sem plano/execução em andamento) interrompe a geração em curso — a da proposta ou a da resposta final — e é confirmado com um `erro` de código `cancelado`. Nenhuma `proposta`/`resposta` atrasada chega depois. Se o cliente desconecta no meio, o daemon cancela a geração sozinho.

**Tempos.** `proposta.tempo_modelo_ms`, `resultado.tempo_execucao_ms` e `resposta.tempo_modelo_ms` são medidos pelo daemon e também registrados no log dele, por etapa; `lia-chat` mostra os três ao concluir o turno.

## 11. Agent Runtime v2: tarefas de vários passos

A seção 10 cobre o turno de **uma** ferramenta. Uma tarefa como "descubra por que o computador está lento" precisa de várias leituras, comparadas entre si. O runtime de tarefas (`sessiond/agente.c`, parte pura; a máquina de execução fica em `sessiond/sessiond.c`) faz:

```
objetivo → plano estruturado → validação independente → execução de passos
        → observação → verificação dos critérios → replanejamento limitado
        → resposta final fundamentada
```

A autonomia vem do runtime (plano limitado, ferramentas tipadas, observação, verificação, replanejamento com orçamento, cancelamento, aprovação por capacidade), não de dar mais liberdade ao modelo.

### 11.1 O plano

```json
{
  "plano_id": "tarefa-…", "hash": "<sha-256 do plano validado>", "versao": 1,
  "objetivo": "investigar por que o sistema pode estar lento agora",
  "intencao": "investigar_lentidao", "compatibilidade": "compativel",
  "passos": [
    { "id": "passo-1", "ferramenta": "sistema.recursos", "parametros": {}, "depende_de": [],
      "objetivo": "carga, memória e swap atuais",
      "evidencia_esperada": ["carga_media", "memoria_disponivel", "swap", "cpu_nucleos"],
      "timeout_ms": 5000, "permitir_falha": false, "somente_leitura": true }
  ],
  "criterios_sucesso": ["carga_media", "memoria_disponivel", "lista_processos_cpu", "…"],
  "capacidades": ["leitura_arquivo", "processo"], "risco": "nenhum",
  "requer_aprovacao": false, "somente_leitura": true,
  "orcamento_passos": 8, "orcamento_tempo_ms": 60000
}
```

**Validação integral, antes de qualquer passo** (`agente_plano_validar`): no máximo 8 passos; ids únicos; toda ferramenta existe no manifesto ativo e tem capacidade permitida; parâmetros válidos; dependências existem e não formam ciclo. Qualquer erro recusa o plano inteiro (`ferramenta_desconhecida`, `capacidade_negada`, `parametro_invalido`, `plano_invalido`) — nenhum passo roda. A validação também calcula o **risco agregado** (o maior entre os passos), a união das capacidades e `requer_aprovacao` (verdadeiro se qualquer passo exigir).

**De onde vem um plano** — sempre validado do mesmo jeito:

- planejador determinístico (`agente_planejar`), para intenções de tarefa reconhecidas: lentidão, comparação CPU × memória, relatório de recursos, consumo anormal, inventário de hardware, estado geral, evidências sobre malware;
- modelo: se a resposta estruturada trouxer `passos`, o plano é validado; plano inválido não é consertado, é rejeitado (`validacao.veredito: "rejeitada"`);
- cliente: `solicitar` com `dados.passos` (mesma forma acima).

**Hash e imutabilidade.** O daemon guarda o plano validado e o envia numa `proposta` (`dados.plano_proposto`). Para executar, o cliente manda `solicitar {"plano_id", "hash"}` — só a referência: o conteúdo executado é o que está guardado no daemon, então nem o cliente nem o modelo conseguem alterá-lo depois de validado. Hash diferente → `plano_alterado`; plano substituído por uma proposta mais nova → `estado_invalido`. **A aprovação é vinculada ao hash**: `aprovar` precisa trazer `plano_id` **e** `hash` do plano apresentado (vale também para o plano de um passo da seção 4, cujo `plano` passou a trazer `hash`); sem hash ou com hash errado → `estado_invalido`.

### 11.2 Máquina de execução

Estados: `PLANO_RECEBIDO → VALIDANDO → AGUARDANDO_APROVACAO → EXECUTANDO → OBSERVANDO → VERIFICANDO → REPLANEJANDO → CONCLUIDO | FALHA | CANCELADO`.

- Um passo começa quando todas as suas dependências terminaram com sucesso; se uma dependência falha, o dependente é marcado `pulado`.
- **Paralelismo só para leitura pura**: passos de leitura sem dependência pendente são despachados juntos (no máximo 4 simultâneos). Um passo que não é leitura pura (escrita, administração, hardware) roda sozinho, nunca em paralelo com nada. As ferramentas `sistema.*` do executor real compartilham buffers estáticos de `cli/sistema.c`, então o executor real as serializa numa fila própria: são despachadas juntas, executadas uma a uma.
- Timeout por passo (`timeout_ms`, padrão do manifesto) e timeout global (`orcamento_tempo_ms`, 60 s). Nenhum passo é despachado duas vezes.
- Cada passo gera `progresso` de início e de fim e uma `auditoria` (mensagem e linha em `~/.lia/humano/auditoria/`, com `tarefa_passo=<id>`).

Mensagens de uma tarefa:

| Mensagem | Conteúdo |
|---|---|
| `plano` | o plano (campos acima) + `tarefa: true` + `execucao_id` da tarefa inteira |
| `progresso` | `dados.tarefa`: `estado`, `fase` (`executando`, `observando`, `verificando`, `replanejando`), `passos_total`, `passos_concluidos`, `replanejamentos`, `tempo_decorrido_ms`; `passo` (índice, ferramenta, estado, `tempo_execucao_ms`, `evidencias`, `limitacoes`, `erro`, `apresentacao`); `plano` (só no replanejamento) |
| `resultado` | `estado`: `sucesso`, `incompleto`, `falha` ou `cancelado`; `dados.tarefa` (plano com a observação de cada passo, inclusive o JSON bruto), `dados.veredito`, `dados.evidencias_ausentes`; `tarefa: true` |
| `resposta` | o relatório final (11.5) |

### 11.3 Observação e verificação

A observação de um passo é o JSON bruto, o texto renderizado dele, as **evidências** que esse JSON sustenta e as limitações. `agente_extrair_evidencias` confere campos, tipos, limites e relações: lista de processos precisa ser uma lista não vazia de itens com `pid` inteiro e métrica inteira; memória disponível não pode passar da total; carga média ausente não vira evidência (vira limitação); lista com menos itens que o pedido é marcada como resultado parcial.

O verificador (`agente_verificar`) compara `criterios_sucesso` com as evidências dos passos concluídos — nunca com um texto do modelo — e responde `SUCESSO`, `REPLANEJAR`, `INCOMPLETO` (há dados, falta evidência, não há mais como buscar) ou `FALHA` (nada foi coletado). Ausência de evidência nunca vira conclusão positiva.

### 11.4 Replanejamento

Só acontece quando falta evidência exigida (ferramenta falhou, dado parcial, dependência não satisfeita). Limites: **3 ciclos**, **8 passos** no total, **60 s** por tarefa.

- Replanejador determinístico: para cada evidência ausente, acrescenta um passo de leitura pura de uma ferramenta que a fornece; a mesma ferramenta com os mesmos parâmetros roda no máximo duas vezes; esgotada uma ferramenta, tenta outra que forneça a mesma evidência.
- Sem candidato determinístico, o modelo pode **sugerir** um passo, uma vez por tarefa. O prompt leva só nomes de evidências e de ferramentas — nunca o conteúdo devolvido pelas ferramentas, que não vira instrução. A sugestão passa pelo validador: precisa existir, ser leitura pura, ter parâmetros válidos e não estar repetida. Sugestão de escrita/administração/hardware é recusada e registrada como limitação.
- Cada ciclo sobe `versao` e recalcula o `hash`. Como o replanejamento só acrescenta leitura pura, nunca muda o risco de um plano já aprovado.
- Orçamento esgotado → `resultado` com estado `incompleto` e uma resposta que começa por `RESULTADO INCOMPLETO`, com motivo, dados coletados, dados ausentes e próximo passo sugerido.

### 11.5 Resposta final

Relatório determinístico (`agente_relatorio`), escrito só a partir das observações: **Conclusão**, **Evidências** (cada linha com a ferramenta de origem entre colchetes), **Limitações**, **Ações realizadas**, **Ações não realizadas**, **Próximo passo**. Os critérios numéricos usados na conclusão são ditos por extenso (carga por núcleo, percentual de memória disponível, swap em uso, um processo ocupando cerca de um núcleo, mais de 25 % da RAM).

O relatório determinístico **é** a resposta (`resposta.origem: "deterministica"`), e sai assim que a coleta termina.

A **leitura do modelo** — até duas frases relacionando as evidências, a partir do relatório entregue como dado delimitado — é opcional e **desligada por padrão**; `LIA_SESSIOND_LEITURA_DO_MODELO=sim` liga. Medido em dez tarefas reais com um modelo local de 3B: a coleta levava ~0,2 s e a leitura, 22 a 42 s; e, mesmo passando na conferência numérica, ela afirmava o que as medições não sustentam ("o sistema está sem problemas", "alta carga de CPU" para um processo com 0,63 %). Ligada, só é anexada (em seção própria, rotulada, `origem: "deterministica+modelo"`) se todo número citado existir nas evidências, se não negar os dados, se não fizer afirmação absoluta de segurança ou infecção e se não concluir que "está tudo bem"; caso contrário é descartada, com `aviso`.

### 11.6 Pedidos sem ferramenta adequada

`validacao.compatibilidade` distingue:

| Valor | Significado |
|---|---|
| `compativel` | há ferramentas que respondem o pedido |
| `parcial` | dá para coletar evidências, mas elas não respondem a pergunta de fundo |
| `inexistente` | nenhuma ferramenta desta versão serve; nada é executado |
| `esclarecer` | o pedido precisa ser esclarecido antes |

"Existe vírus nesta máquina?" recebe `inexistente`: uma resposta direta dizendo que não há ferramenta de detecção de malware, que uma coleta de leitura é possível mas não prova nada, e que nada foi executado — nunca `tools.discover` apresentada como resposta de segurança. "Liste processos suspeitos" é `parcial`: a coleta roda, e o relatório diz que nenhum processo é classificado como vírus nem declarado seguro.

### 11.7 Cancelamento global

`cancelar` com o `execucao_id` da tarefa (ou sem id) interrompe todos os passos em execução, marca os pendentes como `pulado`, cancela qualquer consulta ao modelo e responde um único `resultado` com estado `cancelado`. Nada mais chega depois. Desconexão do cliente tem o mesmo efeito.

### 11.8 Pesquisa web: resposta final conclusiva e estruturada

Uma tarefa cujos passos são `web.pesquisar`/`web.abrir` nunca termina só com a lista de resultados: depois do `resultado`, o daemon sempre envia uma `resposta` que começa por **Resposta:** (a conclusão) e segue com **Pontos principais**, **Linha do tempo** (só quando a pergunta é sobre história/evolução e as fontes têm datas), **O que as fontes não esclarecem**, **Fontes** (numeradas, com título, URL e a data declarada pela página — ou "a página não declara data") e **Como esta resposta foi montada** (`sessiond/sintese_web.c`).

**Coleta.** O planejador (`sessiond/agente.c`) tira da pergunta as palavras de pedido ("me conte com detalhes a…", "por favor") e usa a pergunta anterior do `contexto` só quando a nova é uma continuação ("e quando ela foi lançada?") — nunca o texto da Lia. `web.pesquisar` recebe `abrir_fontes` (padrão 3, máximo 4): as melhores páginas HTTPS públicas, de domínios distintos, são abertas em paralelo; cada resultado lido traz `conteudo` (o trecho mais relevante para a consulta, até 1800 caracteres) e `data`, ou `erro_abertura` quando não abriu. Redirecionamentos (até 3) são seguidos só se o destino passar pela mesma validação de URL pública HTTPS.

**Síntese.** O modelo local recebe os trechos lidos, numerados e delimitados como dados, e devolve JSON com `resposta`, `pontos` (cada um com `fontes`), `linha_do_tempo` e `lacunas`. Antes de exibir, o daemon confere cada item: índices de fonte válidos; o texto precisa ter respaldo lexical na fonte citada (ou é reatribuído à fonte que o sustenta, ou descartado); todo número/ano precisa existir na fonte citada; repetições são removidas. Uma conclusão sem respaldo é trocada pelo primeiro ponto conferido. Itens descartados aparecem em `aviso`.

**Sem modelo utilizável** (não respondeu, texto não é JSON, nada conferiu, ou `LIA_SESSIOND_SINTESE_WEB=extrativa`): a resposta é **extrativa** — frases copiadas dos trechos, escolhidas pela relevância à pergunta, cada uma citada — e diz isso. **Cotação** de moedas é sempre determinística (valor, par, data de referência, fonte, URL). **Sem dados** (rede, timeout, HTTP de erro, busca vazia): a resposta diz o erro real e que nada foi inventado.

`resposta.origem` para pesquisas web: `modelo_sintese`, `extrativa` ou `deterministica` (cotação, e também "sem dados", que vem com `aviso`).

**Andamento.** Enquanto o modelo escreve, o daemon envia `progresso` sem `execucao_id` (a coleta já terminou): `{ "pergunta_id", "fracao", "mensagem", "sintese": { "tokens", "max_tokens", "fontes" } }`, no máximo um por segundo. `cancelar` com `pergunta: true` interrompe a síntese; nenhuma `resposta` chega depois.

**Desempenho medido** (llama3.2:3b em CPU, nesta máquina): coleta + abertura de 3 fontes ~2–3 s; síntese com o modelo já carregado ~50–60 s (prompt ~800–950 tokens em 6–10 s, geração ~300–420 tokens a ~7 tokens/s); primeira pergunta com o modelo frio, +30–90 s. `chat/rodar_local.sh` aquece o modelo ao iniciar o daemon (`LIA_SESSIOND_AQUECER_MODELO=sim`) e as requisições pedem `keep_alive` de 30 min. Limitação conhecida: um modelo de 3B ainda erra relações entre fatos que passam na conferência numérica (por exemplo, juntar dois fatos de anos diferentes numa frase); a conferência garante que números e citações existem nas fontes, não que a frase esteja semanticamente perfeita.

Para depurar a síntese, `LIA_SESSIOND_DEPURAR_SINTESE=1` imprime no log do daemon a saída bruta do modelo antes da conferência.

**Web automática.** Com `LIA_SESSIOND_WEB_AUTOMATICA=sim` (ligada por padrão em `chat/rodar_local.sh`; `LIA_WEB_AUTOMATICA=nao` desliga), `web.pesquisar` e `web.abrir` rodam sem `aprovar`: a política vale só para ferramentas `web.` cuja única capacidade é `rede` (HTTPS externo, nada gravado, nada executado na máquina). Gravação em disco, persistência, administração e execução de programas continuam exigindo aprovação explícita. O daemon marca `"web_automatica": true` em `proposta.dados.validacao` e em `plano`, e o cliente é obrigado a sinalizar o acesso (o lia-chat mostra "🌐 web automática: acessando a internet sem pedir confirmação — <consulta>"). Cada passo é auditado com `aprovacao` = "dispensada: web automática" na mensagem `auditoria` e `aprovacao=dispensada_web_automatica` no log em disco. Sem a variável, o daemon continua exigindo aprovação para toda pesquisa web.

## 12. Arquivos do projeto e boas práticas

Com `LIA_SESSIOND_PROJETO` definido, o executor real oferece as ferramentas
`arquivo.*` (ler, listar, buscar, info — sem aprovação; criar, escrever,
editar, anexar, apagar, mover, copiar, criar_pasta, converter, permissoes,
desfazer — sempre com aprovação) e `praticas.consultar` (catálogo local de
boas práticas, sem aprovação). Detalhes, segurança e exemplos em
`docs/CODING.md`.

Mudanças no protocolo:

- `plano.previa` (ferramenta isolada) e `plano.previas[]` (tarefa:
  `{passo_id, ferramenta, previa, erro?}`): diff unificado do que a escrita
  vai fazer. É o que a pessoa aprova; o cliente deve exibi-lo antes de
  pedir a decisão.
- Uma escrita impossível (trecho ausente/ambíguo, arquivo que já existe,
  fora do projeto, `.git/`) é recusada antes do plano: `erro`
  `parametro_invalido` (ou `capacidade_negada` fora do projeto) para
  `solicitar` direto, ou `proposta` sem plano com o motivo em
  `resposta_texto` no fluxo de pergunta.
- Se os arquivos mudarem entre a prévia e a execução, nada é alterado: o
  passo termina com `plano_alterado` e a `resposta` explica.
- `auditoria.aprovacao`: "aprovada por uma pessoa", "dispensada: web
  automática (...)" ou "não exigida (leitura local)".
- Tarefas de arquivo e de boas práticas terminam com uma `resposta`
  determinística curta (o detalhe — conteúdo, lista, diff, práticas — vem
  na observação do passo).
