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
