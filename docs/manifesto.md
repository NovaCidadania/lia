# Manifesto de ferramentas

Toda ferramenta que a Lia pode executar — por CLI, GUI ou modelo — publica um manifesto. O executor valida o manifesto e a política do usuário **independentemente do modelo**: o modelo escolhe a ferramenta e os parâmetros, mas não decide se ela é permitida.

> **Nota:** o daemon de sessão (`docs/protocolo.md` §8) implementa exatamente estas seis ferramentas de leitura em `sessiond/manifesto_real.c` (o executor padrão). `sessiond/manifesto_mock.c` define, separadamente, uma tabela fixa e fictícia (`mock.leitura`, `mock.escrita`, `mock.admin`, mais `mock.timeout` e `mock.saida_grande`, fixtures só para exercitar o watchdog de timeout e o limite de saída do daemon de verdade) só para testar a máquina de estados do protocolo — ativada apenas com `LIA_SESSIOND_EXECUTOR=mock`, nunca o padrão em produção; não deve ser confundida com as ferramentas reais.

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
  "versao": "0.1.0",
  "parametros": [],
  "capacidades": ["leitura_arquivo", "processo"],
  "risco": "nenhum",
  "efeitos": [
    "lê PATH e diretórios convencionais do sistema",
    "grava ~/.lia/maquina/inventario/ferramentas.lia",
    "grava ~/.lia/humano/diagnosticos/ferramentas.md"
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
  "versao": "0.1.0",
  "parametros": [{ "nome": "safe", "tipo": "booleano", "obrigatorio": true, "padrao": true }],
  "capacidades": ["leitura_arquivo", "processo"],
  "risco": "nenhum",
  "efeitos": [
    "executa, por até 3 s cada, apenas ferramentas do catálogo de probes seguros, com argumento inofensivo e sem privilégio",
    "atualiza ~/.lia/maquina/inventario/ferramentas.lia e o relatório legível"
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

Estes seis manifestos (`tools.discover`, `tools.probe`, `tools.list` acima, mais estes três `sistema.*`) são exatamente os implementados em `sessiond/manifesto_real.c` e expostos pelo executor real do daemon de sessão (`docs/protocolo.md` §8) — a mesma tabela, sem duplicação de lógica.

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

O modelo de linguagem (`docs/protocolo.md` §9, `docs/THREAT-MODEL.md` §5.2) pode sugerir um `objetivo`/`justificativa` e qual ferramenta/parâmetros usar — mas nunca gera um `plano` diretamente: a sugestão (`proposta`) só se torna um `plano` de verdade quando uma pessoa a reenvia como `solicitar`, passando pela mesma validação de manifesto/capacidade/parâmetro desta seção.
