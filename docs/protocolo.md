# Protocolo de eventos — CLI e GUI

Especificação do canal compartilhado entre a CLI, a GUI e o daemon de sessão da Lia (seção 4.1 do [PLANO.md](../PLANO.md)). A GUI é uma camada fina sobre este mesmo protocolo: nenhuma lógica de execução, autorização ou auditoria é duplicada nela.

> Status: especificação para a fase **0.5**. Agora existem os dois lados:
> `gui/protocolo.c` (cliente puro, usado pela GUI — ver [GUI-POC.md](GUI-POC.md))
> e `sessiond/sessiond.c` (**lia-sessiond**, o daemon — servidor de protocolo
> e máquina de estados). **O executor do daemon nesta etapa é inteiramente
> simulado** (`sessiond/manifesto_mock.c`): nenhuma ferramenta real é
> executada, nenhum privilégio é pedido — ver seção 8. A CLI (`lia tools`,
> `lia shell`, `lia sudo`, `lia root`) continua funcionando como processos de
> execução única, sem o socket (seção 6).

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
| `solicitar` | `{ "ferramenta": "tools.discover", "parametros": {...}, "modo": "simular" \| "executar" }` | Pede a execução (ou simulação) de uma ferramenta do manifesto. |
| `aprovar` | `{ "plano_id": "...", "decisao": "aprovar" \| "rejeitar", "escopo": "uma_vez" \| "sessao" }` | Resposta humana a um `plano` pendente. |
| `cancelar` | `{ "execucao_id": "..." }` | Pede cancelamento cooperativo de uma execução em andamento. |
| `assinar` | `{ "canais": ["auditoria", "execucao"] }` | Assina um fluxo de eventos (ex.: a GUI assina `auditoria` para mostrar o histórico ao vivo). |

### Daemon → cliente

| `tipo` | `dados` | Significado |
|---|---|---|
| `plano` | `{ "plano_id", "ferramenta", "parametros_normalizados", "capacidades", "risco", "efeitos", "reversivel", "requer_aprovacao": bool }` | O executor valida o manifesto e descreve o que faria, **antes** de qualquer efeito. |
| `progresso` | `{ "execucao_id", "fracao": 0.0–1.0, "mensagem": "..." }` | Atualização opcional durante uma execução longa. |
| `saida` | `{ "execucao_id", "fluxo": "stdout" \| "stderr", "trecho": "..." }` | Saída incremental de uma ferramenta que produz texto. |
| `resultado` | `{ "execucao_id", "estado": "sucesso" \| "falha" \| "cancelado", "codigo_saida": int, "dados": {...} }` | Resultado final, sempre emitido mesmo em erro ou cancelamento. |
| `auditoria` | `{ "entrada_id", "ts", "identidade", "ferramenta", "parametros_normalizados", "capacidades", "resultado_resumo", "reversao_disponivel": bool }` | Eco do registro gravado em `~/.lia/humano/auditoria/`; é sempre gravado antes de ser emitido. |
| `erro` | `{ "codigo", "mensagem", "detalhe" }` | Falha do protocolo ou do daemon (não confundir com `resultado.estado = "falha"`, que é falha da ferramenta). |

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

- **`real`** (`sessiond/executor_real.c`/`sessiond/manifesto_real.c`, padrão em produção) — chama `cli/sistema.c` e `cli/ferramentas.c` de verdade, limitado às seis ferramentas de leitura: `tools.discover`, `tools.list`, `tools.probe`, `sistema.diagnosticar`, `sistema.recursos`, `sistema.dispositivos`. Todas `risco: nenhum`, `requer_aprovacao: false` — nenhuma delas chega ao estado "aguardando aprovação". A única execução de processo externo em todo o backend é `tools.probe --safe`, pelo catálogo fixo já existente em `cli/ferramentas.c` (sandboxed, com timeout, sem shell) — exceção escopada e documentada em `docs/THREAT-MODEL.md` §5.1; mesmo essa ferramenta rejeita `{"safe": false}` com `parametro_invalido`, porque "probe sem --safe" não existe nesta etapa. `modo: "executar"` e `modo: "simular"` em `solicitar` são tratados da mesma forma: as seis ferramentas sempre leem de verdade (são leitura, sem efeito a simular).
- **`mock`** (`sessiond/executor_mock.c`/`sessiond/manifesto_mock.c`, só via `LIA_SESSIOND_EXECUTOR=mock`) — tabela fixa e fictícia (`mock.leitura`, `mock.escrita` risco médio, `mock.admin` risco admin) só para testar a máquina de estados inteira (plano → aprovação → progresso/saída → resultado → auditoria, replay, cancelamento) sem nenhum efeito real em nenhum caso — nem `mock.admin` jamais chama `sudo`/`pkexec`. **Nunca é o executor padrão em produção** (`sessiond.c`, `escolher_executor`).

Validação de parâmetros (`validar_parametros`) e a política "só capacidades de leitura" (`Executor.restringe_a_leitura`, verificada em `tratar_solicitar` antes de qualquer execução) são do daemon, nunca da GUI — a GUI só reflete o que o daemon decidiu.
