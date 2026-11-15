# Manifesto de ferramentas

Toda ferramenta que a Lia pode executar — por CLI, GUI ou modelo — publica um manifesto. O executor valida o manifesto e a política do usuário **independentemente do modelo**: o modelo escolhe a ferramenta e os parâmetros, mas não decide se ela é permitida.

> **Nota:** o daemon de sessão (`docs/protocolo.md` §8) implementa estas sete ferramentas de leitura em `sessiond/manifesto_real.c` (o executor padrão), mais duas ações explícitas de persistência (`tools.discover.persistir`, `tools.probe.persistir` — §6.1). `sessiond/manifesto_mock.c` define, separadamente, uma tabela fixa e fictícia (`mock.leitura`, `mock.escrita`, `mock.admin`, mais `mock.timeout` e `mock.saida_grande`, fixtures só para exercitar o watchdog de timeout e o limite de saída do daemon de verdade) só para testar a máquina de estados do protocolo — ativada apenas com `LIA_SESSIOND_EXECUTOR=mock`, nunca o padrão em produção; não deve ser confundida com as ferramentas reais.

## 1. Campos obrigatórios

| Campo | Tipo | Descrição |
|---|---|---|
| `nome` | string | Identificador estável, com namespace por domínio: `tools.discover`, `files.ler`, `shell.abrir`, `sudo.executar`, `root.comando`. |
| `versao` | string (semver) | Versão do manifesto/implementação. Mudança de comportamento observável exige incremento de versão. |
| `parametros` | lista de `{ nome, tipo, obrigatorio, padrao, limites }` | Tipos primitivos (`string`, `inteiro`, `booleano`, `caminho`, `enum`) ou lista deles. `limites` descreve faixa, tamanho máximo ou padrão (regex) quando aplicável. |
| `capacidades` | lista de enum | O que a execução exige: `leitura_arquivo`, `escrita_arquivo`, `processo` (criar/encerrar), `rede_local`, `rede_externa`, `entrada_dispositivo` (teclado/mouse), `captura_tela`, `hardware`, `admin` (root/Admin). |
| `risco` | enum | `nenhum` (somente leitura, sem efeito observável fora do processo), `baixo` (efeito local reversível, ex.: criar arquivo novo), `medio` (efeito que altera estado existente, ex.: sobrescrever arquivo), `alto` (efeito amplo ou difícil de reverter), `admin` (exige privilégio elevado, classificado independentemente do impacto do comando em si). |
| `efeitos` | lista de string | Descrição legível e específica do que muda no sistema (ex.: "grava `~/.lia/maquina/inventario/ferramentas.lia`", "inicia um processo filho temporário"). Nunca "diversos" ou vazio. |
| `idempotente` | booleano | Repetir com os mesmos parâmetros produz o mesmo efeito líquido (sem duplicar nem acumular). |
| `timeout_padrao_ms` | inteiro | Limite de tempo da execução; o chamador pode reduzir, nunca aumentar além de um `timeout_maximo_ms` do manifesto. |
| `limites` | objeto | Outros tetos aplicáveis: tamanho máximo de saída capturada, número máximo de itens, profundidade de recursão. |
| `reversivel` | enum | `sim` (a Lia sabe desfazer e registra o necessário em `~/.lia/maquina/reversao/`), `nao_aplicavel` (risco `nenhum`, não há o que desfazer), `nao` (efeito definitivo — exige risco `alto` ou `admin` e justificativa explícita no `plano`). |
| `requer_aprovacao` | booleano | Derivado de `risco`: `false` somente quando `risco = nenhum`. Todo o resto é `true`, mesmo com uma política de auto-aprovação configurada (a política fica registrada como a autorização usada). |

## 2. Classificação de poder (alinhada a `~/.lia/humano/permissoes.toml`)

| Nível | Quem pode chamar sem pedir de novo | Exemplo |
|---|---|---|
| `leitura` | Qualquer política | `tools.discover`, `tools.list`, `tools.probe`, `sistema.diagnosticar` |
| `usuario` | Política `usuario` ou mais alta | `files.ler`, `shell.abrir` (lê e roda no contexto do próprio usuário) |
| `admin` | Política `admin`, com aprovação por ação | `sudo.executar`, `root.comando`, alterações em arquivos de configuração do sistema |
| `hardware` | Política `hardware`, com aprovação por ação | acesso a `/dev/input`, `/dev/uinput`, captura de tela, sensores |

## 3. Exemplos — ferramentas já implementadas (fase 0.5, somente leitura)

```json
{
  "nome": "tools.discover",
  "versao": "0.2.0",
  "parametros": [],
  "capacidades": ["leitura_arquivo", "processo"],
  "risco": "nenhum",
  "efeitos": [
    "lê PATH e diretórios convencionais do sistema",
    "não grava nada (ver tools.discover.persistir, §6.1)"
  ],
  "idempotente": true,
  "timeout_padrao_ms": 30000,
  "limites": { "max_ferramentas": 16384 },
  "reversivel": "nao_aplicavel",
  "requer_aprovacao": false
}
```

```json
{
  "nome": "tools.probe",
  "versao": "0.2.0",
  "parametros": [{ "nome": "safe", "tipo": "booleano", "obrigatorio": true, "padrao": true }],
  "capacidades": ["leitura_arquivo", "processo"],
  "risco": "nenhum",
  "efeitos": [
    "executa, por até 3 s cada, apenas ferramentas do catálogo de probes seguros, com argumento inofensivo e sem privilégio",
    "não grava nada (ver tools.probe.persistir, §6.1)"
  ],
  "idempotente": true,
  "timeout_padrao_ms": 3000,
  "limites": { "max_saida_bytes": 4096 },
  "reversivel": "nao_aplicavel",
  "requer_aprovacao": false
}
```

```json
{
  "nome": "tools.list",
  "versao": "0.1.0",
  "parametros": [
    { "nome": "estado", "tipo": "enum", "obrigatorio": false },
    { "nome": "json", "tipo": "booleano", "obrigatorio": false, "padrao": false }
  ],
  "capacidades": ["leitura_arquivo"],
  "risco": "nenhum",
  "efeitos": ["lê ~/.lia/maquina/inventario/ferramentas.lia; não escreve nada"],
  "idempotente": true,
  "timeout_padrao_ms": 2000,
  "limites": {},
  "reversivel": "nao_aplicavel",
  "requer_aprovacao": false
}
```

```json
{
  "nome": "sistema.diagnosticar",
  "versao": "0.1.0",
  "parametros": [],
  "capacidades": ["leitura_arquivo"],
  "risco": "nenhum",
  "efeitos": ["lê /etc/os-release, /proc/cpuinfo, /proc/meminfo, uname; não escreve nada"],
  "idempotente": true,
  "timeout_padrao_ms": 5000,
  "limites": {},
  "reversivel": "nao_aplicavel",
  "requer_aprovacao": false
}
```

```json
{
  "nome": "sistema.recursos",
  "versao": "0.1.0",
  "parametros": [],
  "capacidades": ["leitura_arquivo"],
  "risco": "nenhum",
  "efeitos": ["lê /proc/meminfo, /proc/cpuinfo, /proc/uptime; não escreve nada"],
  "idempotente": true,
  "timeout_padrao_ms": 5000,
  "limites": {},
  "reversivel": "nao_aplicavel",
  "requer_aprovacao": false
}
```

```json
{
  "nome": "sistema.dispositivos",
  "versao": "0.1.0",
  "parametros": [],
  "capacidades": ["leitura_arquivo"],
  "risco": "nenhum",
  "efeitos": ["lê /sys/class/net, /sys/class/block, /sys/bus/pci/devices; não escreve nada"],
  "idempotente": true,
  "timeout_padrao_ms": 5000,
  "limites": {},
  "reversivel": "nao_aplicavel",
  "requer_aprovacao": false
}
```

```json
{
  "nome": "sistema.processos_top_cpu",
  "versao": "0.1.0",
  "parametros": [
    { "nome": "limite", "tipo": "inteiro", "obrigatorio": false, "padrao": 10, "limites": { "min": 1, "max": 100 } },
    { "nome": "intervalo_ms", "tipo": "inteiro", "obrigatorio": false, "padrao": 200, "limites": { "min": 100, "max": 5000 } },
    { "nome": "ordenar_por", "tipo": "enum", "obrigatorio": false, "padrao": "cpu", "limites": { "valores": ["cpu"] } }
  ],
  "capacidades": ["leitura_arquivo", "processo"],
  "risco": "nenhum",
  "efeitos": [
    "lê /proc/<pid>/stat de cada processo (duas amostras, com uma pausa curta entre elas, nunca cmdline/environ)",
    "não escreve nada"
  ],
  "idempotente": false,
  "timeout_padrao_ms": 8000,
  "limites": {},
  "reversivel": "nao_aplicavel",
  "requer_aprovacao": false
}
```

`sistema.processos_top_cpu` responde perguntas como "liste os 10 processos que mais estão usando CPU": amostra `/proc/<pid>/stat` duas vezes, com `intervalo_ms` de pausa entre as amostras, e calcula o percentual de CPU de cada processo no intervalo, normalizado pelo número de núcleos (um processo usando 100% de todos os núcleos aparece como 100%, nunca `100% × núcleos`). `idempotente: false` porque, ao contrário dos outros seis manifestos desta seção, o resultado depende do instante da amostragem — repetir a chamada não reproduz os mesmos números, mesmo sem nenhum efeito colateral.

Nome e estado de processo já podem revelar informação sensível sobre o que a pessoa está rodando (ver `docs/THREAT-MODEL.md` §5.1) — por isso esta ferramenta nunca lê `/proc/<pid>/cmdline` nem `/proc/<pid>/environ` (argumentos completos, variáveis de ambiente, tokens e caminhos que poderiam vir embutidos neles) nesta primeira versão, mesmo sendo `risco: nenhum`. `capacidades` inclui `"processo"` (além de `"leitura_arquivo"`, já que a fonte é `/proc`) para o manifesto declarar com precisão o que de fato é lido — a mesma capacidade que `tools.discover`/`tools.probe` já usam para o catálogo de processos/binários.

```json
{
  "nome": "sistema.processos_top_memoria",
  "versao": "0.1.0",
  "descricao": "Lista os processos que mais usam memória RAM (RSS) agora.",
  "parametros": {
    "limite": { "tipo": "inteiro", "min": 1, "max": 100, "padrao": 10 },
    "ordenar_por": { "tipo": "enum", "valores": ["memoria"], "padrao": "memoria" }
  },
  "capacidades": ["leitura_arquivo", "processo"],
  "risco": "nenhum",
  "efeitos": ["lê /proc/<pid>/status de cada processo e /proc/meminfo; não lê cmdline/environ; não escreve e não inicia processos"],
  "requer_aprovacao": false,
  "idempotente": false,
  "timeout_padrao_ms": 5000,
  "reversivel": "nao_aplicavel"
}
```

`sistema.processos_top_memoria` responde "liste os processos que mais usam memória RAM". Uma única passada por `/proc`:

- **Fonte: `/proc/<pid>/status` (campos `Name`, `State`, `VmRSS`), não `/proc/<pid>/statm`.** `status` já traz o RSS em kB, rotulado pelo kernel; `statm` traz *páginas*, e converter exige o tamanho de página — que, sem libc (`cli/sistema.c` não tem `sysconf`), teria de ser presumido como 4096, premissa falsa em kernels com página de 16K/64K. `status` também distingue thread de kernel (sem linha `VmRSS`) de processo com RSS zero.
- **Saída (JSON estável, só inteiros):** `ordenar_por`, `limite`, `considerados` (processos com memória de usuário), `sem_memoria_de_usuario` (threads de kernel/zumbis, contadas e deixadas fora), `memoria_total_kb` (`MemTotal`) e `processos[]` com `pid`, `nome`, `estado`, `rss_kb`, `mem_pct_centesimos` (percentual da RAM × 100 — centésimos, nunca ponto flutuante, pelo mesmo motivo de `cpu_pct_centesimos`). Ordenado por `rss_kb` decrescente; empate por `pid` crescente.
- **Nunca** lê `/proc/<pid>/cmdline` nem `/proc/<pid>/environ`, nunca executa comando externo, nunca usa shell, nunca exige root. Processo que desaparece entre listar e ler é ignorado, sem falhar a coleta.
- **Limites:** no máximo 8192 processos considerados (vetor estático, sem alocação dinâmica), no máximo 100 devolvidos, 5 s de execução (watchdog do daemon).
- Saída humana pela CLI: `lia system processos_top_memoria [--limite N]`.

`sistema.recursos` também devolve `carga_media_centesimos` (`1min`/`5min`/`15min`, de `/proc/loadavg`) e `uptime_segundos`; `sistema.dispositivos` aceita o parâmetro opcional `classe` (`rede`, `bloco` ou `pci`) para devolver só uma classe.

### Roteamento: validação independente da ferramenta proposta

`sessiond/roteador.c` (código puro, testado em `testes/unit/teste_roteador.c`) fica **entre o modelo e o executor**:

```
pedido da pessoa → intenção classificada → ferramenta candidata → validação independente → (só então) plano/execução
```

- **Classificação por intenção, não por palavra solta.** Combina conceitos do pedido: "processo" + "memória" → `processos_por_memoria`; "o que/quem mais consome CPU" → `processos_por_cpu`; "quanta memória livre" (sem processo, sem ranking) → `recursos_do_sistema`; "modelo do processador" não é confundido com "processo".
- **Cada intenção tem a lista das ferramentas compatíveis.** Intenção de processos nunca aceita `sistema.dispositivos` nem `sistema.recursos`; processos por memória só aceita `sistema.processos_top_memoria`. Proposta incompatível do modelo é substituída pela ferramenta da rota (veredito `corrigida`), e a `proposta` registra o que o modelo tinha dito (`validacao.ferramenta_do_modelo`).
- **Ferramenta inexistente nunca passa**; sem rota que a corrija, o modelo recebe **uma** nova tentativa com o motivo da rejeição e, se insistir, a proposta sai sem ferramenta (veredito `rejeitada`).
- **Parâmetro inválido nunca é consertado por palpite**: volta ao padrão do manifesto ou a proposta é rejeitada. Um inteiro que o modelo colocou sem que a pessoa tenha dito aquele número (o modelo local inventou `"limite": 100` em teste) é descartado. O limite só vale quando a própria pessoa o disse ("os 5 processos", "top 5").
- **Intenção reconhecida sem ferramenta** (ex.: espaço livre em disco) remove a proposta (veredito `sem_ferramenta`) em vez de reaproveitar uma ferramenta parecida; **pedido ambíguo** (CPU e memória ao mesmo tempo) vira pergunta de esclarecimento.
- **Rota direta** (`LIA_SESSIOND_ROTEAMENTO=direto`, padrão): para intenção de confiança alta, a proposta sai na hora pela rota determinística, sem consultar o modelo para escolher a ferramenta (medido: o modelo local levava ~41 s só nessa etapa). `LIA_SESSIOND_ROTEAMENTO=modelo` faz o modelo sempre propor e o roteador validar/corrigir. A decisão passa pela mesma função nos dois modos.

Os oito manifestos locais (`tools.discover`, `tools.probe`, `tools.list`, `sistema.diagnosticar`, `sistema.recursos`, `sistema.dispositivos`, `sistema.processos_top_cpu`, `sistema.processos_top_memoria`), `web.pesquisar`, `web.abrir` e as duas ações de persistência da §6.1 são exatamente os implementados em `sessiond/manifesto_real.c` e expostos pelo executor real do daemon de sessão (`docs/protocolo.md` §8). As ferramentas web têm capacidade `rede`, risco `baixo` e aprovação obrigatória: pesquisa e abertura enviam/acessam conteúdo externo por HTTPS. `web.abrir` aceita páginas públicas, sem credenciais, sem JavaScript e com limite de conteúdo; redes privadas, loopback e redirecionamentos não validados são recusados. Cada ferramenta declara também `somente_leitura` (`sessiond/executor.h`): só as que são leitura pura podem seguir sem confirmação no piloto automático (`docs/protocolo.md` §10); `tools.probe` é `risco: nenhum` mas executa binários do catálogo de probes, então **não** é leitura pura.

## 4. Ferramentas de nível `admin` (já implementadas, fora do escopo "somente leitura")

`shell.abrir` (`lia shell`), `sudo.executar` (`lia sudo`) e `root.comando` (`lia root`) existem desde a fase anterior. Elas **não guardam senha nem alteram `/etc/sudoers`**: cada chamada usa o `sudo` real do sistema, pede autenticação quando o `sudo` pedir, e é registrada em `~/.lia/humano/auditoria/` antes de executar. No vocabulário desta seção:

- `risco`: `admin`.
- `reversivel`: `nao` (a execução de um comando arbitrário como root não tem reversão genérica).
- `requer_aprovacao`: `true` — hoje a aprovação é o próprio ato de a pessoa digitar o comando na CLI; quando o protocolo (seção [protocolo.md](protocolo.md)) e a GUI existirem, a mesma chamada passa a gerar um evento `plano` explícito antes de executar, mesmo vindo da CLI.

Nenhuma ferramenta neste projeto tem `capacidades: ["admin"]` com `requer_aprovacao: false`.

## 5. O que o executor rejeita antes de rodar qualquer ferramenta

- `ferramenta` fora do manifesto carregado → `ferramenta_desconhecida`.
- Parâmetro fora do tipo/limite declarado → `parametro_invalido`.
- `capacidades` pedidas além do nível liberado em `~/.lia/humano/permissoes.toml` → `capacidade_negada`.
- `risco` acima de `nenhum` sem um evento `aprovar` correspondente → `aprovacao_pendente`.
- Execução além de `timeout_padrao_ms` (ou do `timeout` pedido, se menor) → encerrada e reportada como `limite_excedido` (`sessiond/sessiond.c`, `ao_timeout_execucao` — cooperativo: o cliente recebe a resposta de imediato, o trabalho em si pode levar um instante extra para realmente parar).
- `aprovar` repetido para um plano já decidido (replay) → `estado_invalido`: o estado da sessão já saiu de "aguardando aprovação" na primeira vez.
- `aprovar` referenciando um `plano_id` diferente do plano efetivamente apresentado → `estado_invalido`.
- `cancelar` sem `execucao_id`, ou com um que não corresponde à execução em andamento da própria conexão → `estado_invalido` (nunca cancela a execução errada por engano).

Esta validação é do executor, não do modelo: um modelo comprometido ou um arquivo malicioso interpretado como instrução não pode se autorizar.

## 6. Manifesto (o que uma ferramenta declara) vs. plano (o que uma execução concreta propõe)

Este documento descreve o **manifesto**: a declaração estática de uma ferramenta, carregada uma vez. Uma execução concreta gera, em cima dele, um **plano** (mensagem `plano` — `docs/protocolo.md` §3), com campos próprios dessa instância: `plano_id`, `versao_manifesto` (cópia do `versao` do manifesto no momento do plano, para detectar manifesto trocado entre o plano e a aprovação), `objetivo`/`justificativa` (opcionais, descrevem o "porquê" desta chamada específica), `passos` (lista — hoje sempre um único passo nesta etapa), e os campos do próprio manifesto (`capacidades`, `risco`, `efeitos`, `idempotente`, `timeout_padrao_ms`, `reversivel`, `requer_aprovacao`) ecoados para a pessoa decidir sem precisar confiar em memória.

O modelo de linguagem (`docs/protocolo.md` §9, `docs/THREAT-MODEL.md` §5.2) pode sugerir um `objetivo`/`justificativa` e qual ferramenta/parâmetros usar — mas nunca gera um `plano` diretamente: a sugestão (`proposta`) só se torna um `plano` de verdade quando é reenviada como `solicitar` (pela pessoa, ou pelo piloto automático quando o daemon a marcou como leitura pura), passando pela mesma validação de manifesto/capacidade/parâmetro desta seção. A mensagem `plano` traz um `hash`, e `aprovar` precisa citá-lo.

## 6.0 Plano de vários passos (Agent Runtime v2)

Uma tarefa composta gera um plano com até **8 passos** (`docs/protocolo.md` §11). Cada passo declara `id`, `ferramenta`, `parametros`, `depende_de`, `objetivo`, `evidencia_esperada`, `timeout_ms` e `permitir_falha`; o plano declara `objetivo`, `criterios_sucesso`, `orcamento_passos` (8) e `orcamento_tempo_ms` (60000). O que o manifesto de cada ferramenta determina no plano:

- `risco` do plano = o maior `risco` entre os passos; `capacidades` = a união; `requer_aprovacao` = verdadeiro se **qualquer** passo exigir;
- só passos de ferramenta `somente_leitura` rodam em paralelo; qualquer outro roda sozinho;
- o replanejamento só pode acrescentar ferramentas `somente_leitura`;
- o plano validado recebe um `hash` (SHA-256); execução e aprovação ficam presas a ele.

**Evidências por ferramenta** (o que o verificador aceita, conferindo campos e tipos do JSON):

| Ferramenta | Evidências |
|---|---|
| `sistema.recursos` | `memoria_total`, `memoria_disponivel`, `swap`, `uptime`, `carga_media`, `cpu_nucleos`, `cpu_modelo` |
| `sistema.diagnosticar` | `distribuicao`, `kernel`, `cpu_modelo`, `cpu_nucleos`, `memoria_total`, `memoria_disponivel` |
| `sistema.processos_top_cpu` | `lista_processos_cpu` |
| `sistema.processos_top_memoria` | `lista_processos_memoria`, `memoria_total` |
| `sistema.dispositivos` | `dispositivos_rede`, `dispositivos_bloco`, `dispositivos_pci` (só a classe pedida, quando há `classe`) |
| demais | `resultado:<ferramenta>` |

Não existe, nesta versão, nenhuma ferramenta de detecção de malware, de pacotes instalados, de arquivos alterados, de serviços ou de integridade. Pedidos que dependem delas recebem compatibilidade `inexistente` ou `parcial` — nunca uma ferramenta parecida no lugar.

## 6.1 Descoberta vs. persistência: `tools.discover`/`tools.probe` nunca gravam por si só

Auditoria pós-commit `e366f2b` (ponte com o modelo) encontrou uma inconsistência real: `tools.discover` e `tools.probe` estavam classificados `risco: nenhum`/`requer_aprovacao: false` ("sem efeito observável fora do processo" — §1), mas `cli/ferramentas.c` gravava, em toda chamada, `~/.lia/maquina/inventario/ferramentas.lia` e `~/.lia/humano/diagnosticos/ferramentas.md` — um efeito bem observável fora do processo.

A correção separa descoberta de persistência, em vez de passar as duas ferramentas a exigir aprovação (o que quebraria a GUI hoje: `gui/protocolo.c` nunca envia `aprovar`, por desenho, enquanto `modo_leitura` estiver ativo — ver `docs/THREAT-MODEL.md` §5.1):

- `tools.discover`/`tools.probe` (inalteradas para quem já as chama, GUI incluída): `cli/ferramentas.c` roda com a nova flag `--sem-salvar` — fazem a varredura/probe e reportam o resultado, mas **nunca gravam nada em disco**. Continuam `risco: nenhum`, `requer_aprovacao: false`, honestamente agora.
- `tools.discover.persistir`/`tools.probe.persistir` (novas): repetem a mesma operação, mas SEM `--sem-salvar` — gravam os dois arquivos de verdade. `risco: baixo` (escreve fora do processo, mas só no espaço de trabalho da própria Lia — inventário e diagnóstico derivados, nunca conteúdo do usuário), `requer_aprovacao: true` sempre.

Quem precisa que `tools.list` funcione (ela só lê o inventário já persistido) precisa primeiro rodar `tools.discover.persistir` — uma decisão explícita e aprovada, nunca um efeito colateral escondido de uma ferramenta "somente leitura".
