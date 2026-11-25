# Avaliação independente v1 (2026-11-25)

Objetivo: medir se a Lia, com cada modelo disponível, **resolve requisitos
de código** — não se as suítes passam. Esta avaliação **não promove nenhum
modelo**: ver "Autoria" abaixo.

## Autoria e o que isso limita

As 6 tarefas e seus oráculos foram escritas na mesma sessão que mantém o
runtime. Isso quebra a independência: quem escreve o teste conhece as
fraquezas do código. Mitigações aplicadas (e que **não** a tornam cega):

- os enunciados trazem **critérios de aceitação visíveis**; o oráculo só cobra
  o que está neles (conferido item a item ao escrever);
- cada oráculo foi executado **antes** de qualquer execução do agente, dentro
  do mesmo bubblewrap usado na avaliação: **reprova no código original** (rc 1)
  e **aprova numa solução de referência** (rc 0; a referência não é
  versionada) — as 6 tarefas, Python e Node;
- o código dos oráculos fica fora do repositório
  (`~/.lia/avaliacao-oculta/2026-11-25-independente-v1.json`, 0600); o
  repositório só tem o `sha256` e o avaliador recusa rodar se o hash não
  bater; resultados não gravam o código; a Lia nunca o vê (rodam depois, num
  clone, em `.lia/`, que as aprovações do avaliador recusam);
- nada deste conjunto pode ser usado para ajustar prompt, regra ou modelo.

Para ser de fato independente, **outra pessoa** precisa escrever tarefas e
oráculos sem ver o runtime. O formato está pronto: acrescente tarefas ao
JSON (`projeto`, `prompt` com critérios, `verificacao_oculta {tipo, sha256}`)
e o código do oráculo em `~/.lia/avaliacao-oculta/<mesmo nome do JSON>`
(`tipo`: `python` — script que sai com 1 se falhar; `node` — arquivo
`node:test`; `vitest`). Fixtures vêm de `ferramentas/avaliacao/criar-fixtures.sh`.

## Tarefas

| id | projeto | dificuldade | o que pede |
|---|---|---|---|
| ind-py-1 | inventario-py (Python, unittest) | fácil | validar entrada e arredondar `aplicar_desconto` |
| ind-py-2 | inventario-py | médio | `Estoque.remover` recusa retirada inválida, sem mudar o estado |
| ind-py-3 | inventario-py | difícil | `abaixo_do_minimo` + seção no relatório (2 arquivos, formato exato) |
| ind-js-1 | agenda-js (Node, node:test) | fácil | `parseDataBR` rejeita datas inexistentes/mal formadas |
| ind-js-2 | agenda-js | médio | `Fila.proximo` por prioridade, estável |
| ind-js-3 | agenda-js | difícil | `parseCsv` com aspas, `""`, `\r\n` |

Projetos usam só a biblioteca padrão (nada a instalar). Commits-base
determinísticos: `inventario-py` `e76ed930930f`, `agenda-js` `5eae349bc108`.

## Condições e protocolo

- `sem-modelo` (baseline: o mock não propõe edição), `llama3.2:3b` (Ollama,
  digest `a80c4f17…`), `Qwen2.5-Coder-7B-Instruct-Uncensored Q5_K_M`
  (llama-server, sha256 `cb8f52cc…`).
- Temperatura 0.4 e sementes 1, 2, 3 (3 repetições por tarefa e condição);
  parâmetros e hash do binário (`lia_sessiond_sha256`) vão em cada resultado.
- Cada execução: clone novo do repositório-base, política de aprovação fixa
  do avaliador, suíte antes/depois, oráculo, original intocado.
- Classes: `sucesso` (concluiu **e** o oráculo passou **e** a suíte depois
  passou **e** nenhuma violação), `concluido_falso`, `nao_concluido`,
  `bloqueado por ambiente`, `sem_oraculo` (não avaliável). Contagens, não
  porcentagens: o N é pequeno demais para intervalos de confiança úteis.
- Nenhuma mudança de runtime, prompt, avaliador ou critério durante uma
  matriz. Falha não é motivo de repetir; repetir exige causa identificada e
  nova condição declarada antes.

## Qwen: procedência e licença (conferido)

O GGUF (`~/models/Qwen2.5-Coder-7B-Instruct-Uncensored.Q5_K_M.gguf`) é uma
quantização feita por terceiros (`mradermacher`) de um fine-tune de
terceiros (`thirdeyeai/Qwen2.5-Coder-7B-Instruct-Uncensored`). O arquivo
**não declara licença**. "Uncensored" não é evidência de qualidade de código
nem de licença compatível. Portanto **não é candidato oficial**: serve só de
ponto de comparação até que a licença do fine-tune seja verificada na fonte
(não feito: exigiria acesso à rede).

## Harness corrigido (2026-11-25, depois da primeira tentativa)

A primeira execução desta matriz foi **interrompida e invalidada**: o avaliador não tratava `proposta` com
`plano_proposto` (só reagia a `plano`), então `ind-py-2` esperou 40 min sem avançar o protocolo. Era defeito do
harness, não do modelo. Os 13 resultados parciais (todos do baseline sem modelo) foram movidos para
`~/.lia/avaliacao-resultados/independente-v1-INVALIDO-handshake/` com `INVALIDO.txt`; **não entram em nenhuma comparação**.

Correções (`ferramentas/avaliacao/`):

- `protocolo.py`: sequência real `proposta(plano_proposto)` → `solicitar` (preso a `plano_id`/`hash`, `origem_modelo`,
  `pergunta_id`) → `plano` → `aprovar` (mesmo hash) → `resultado` → `resposta`. Silêncio (`sem_progresso`; 90 s sem modelo, 720 s
  com modelo, configurável), desconexão, `erro` do daemon, evento desconhecido e plano reproposto são **erros terminais de
  harness com diagnóstico** (classe `erro_de_harness`, arquivo `*.harness-erro.json`, exit 3) — nunca falha nem sucesso do modelo.
  `limite_tempo` com eventos chegando continua sendo `nao_concluido` (o runtime/modelo não terminou).
- `avaliar.py` (`avaliar/3`): limpeza da árvore do daemon (que abre sessão própria) em saída normal, exceção e
  SIGTERM/SIGINT/SIGHUP; resultado gravado de forma atômica; `estado_terminal`, `avaliador_versao` e `avaliador_sha256` em cada
  resultado; `--versao`; `--reter-rejeitadas` (opt-in, só fixtures sintéticas).
- `matriz.py`: fail-fast e retomável. Confere código de saída **e** o JSON; só reaproveita resultado cujos metadados
  (tarefa, condição, semente, hash do modelo, parâmetros, hash do arquivo de tarefas, hash do binário, versão e hash do
  avaliador, estado terminal) conferem; senão descarta e refaz. `manifesto.json` (atômico) com esperadas, válidas, faltantes,
  falhas de harness e totais por condição. **`FIM` só sai com todas as combinações terminais válidas**; senão `INCOMPLETO`
  (exit 1); interrompido: `INTERROMPIDO` (exit 130) e toda a descendência é encerrada.
- `independente-v1.sh` chama o `matriz.py` (54 combinações). Use um diretório de saída **novo**.

Teste obrigatório antes de qualquer matriz: `make teste-harness-avaliacao` (daemon falso para o protocolo, caso curto real
`ind-py-2` sem modelo, limpeza de processos, runner). Resultado desta rodada: 35 testes, 0 falhas; o caso que esperava 40 min
termina em ~6 s.

## Runtime: o que mudou junto (causas gerais, não casos)

- **Proibições**: padrões por substring (`*email*`) valiam para código-fonte e escondiam `emails_send.py`. Agora as
  categorias ("não leia os emails") só cobrem arquivos de **dados** (extensão de dados/configuração); caminhos são
  normalizados (`./`, `..`, `//`) e há checagem pelo caminho real (link simbólico para arquivo proibido).
- **Critério `teste_cobre`**: o pedido que manda acrescentar um teste e cita um símbolo só é cumprido se um teste gravado
  **cita esse símbolo**; teste criado não prova o requisito por existir.
- **Reparo**: a falha de validação vai ao modelo como diagnóstico mínimo (testes, arquivo:linha, mensagens) mais o final da
  saída, não a saída inteira. Proposta idêntica sem progresso continua recusada e o limite de tentativas encerra como "NÃO concluído".
- **Retenção opt-in** (`LIA_SESSIOND_RETER_REJEITADAS=<dir>` + marca `.lia/fixture-sintetica`, que só o avaliador cria para
  fixtures sintéticas): `rejeitadas.jsonl` 0600 com hash do contexto, resposta, motivo e resultado da validação. Desligada por padrão.

O oráculo continua obrigatório para declarar sucesso; suíte visível verde ou teste recém-criado não bastam.
