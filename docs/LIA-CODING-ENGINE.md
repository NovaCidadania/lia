# Lia Coding Engine

O "modelo próprio" da Lia, nesta etapa, é um **produto versionado**: runtime
e protocolo de coding, árvore de tarefas com evidências, trajetórias
verificadas e um pipeline para adaptar um modelo aberto **se e quando**
houver dados e hardware. Não é um LLM treinado do zero, e **nenhum modelo
foi treinado** até aqui (ver `docs/TREINO.md`).

Decisões: `docs/adr/0001-coding-minimo-runtime.md` (o runtime decide) e
`docs/adr/0002-lia-coding-engine.md` (protocolo, árvore, dados, promoção).

## 1. Protocolo da proposta

| Operação | Obrigatórios | Proibidos | Limites |
|---|---|---|---|
| `criar` | `caminho`, `conteudo` (não vazio) | campos de outras operações e desconhecidos | conteúdo ≤ 128 KiB, UTF-8, não abreviado |
| `editar` | `caminho`, `procurar` (não vazio), `substituir` (pode ser vazio) | idem | procurar ≤ 16 KiB; ocorrência única no arquivo lido |
| `substituir_funcao` | `caminho`, `funcao` (só o nome, ≤ 120), `conteudo` (a função inteira) | idem | região achada no arquivo lido (chaves ou recuo Python) |

- `coding_contrato_edicao` confere **toda** proposta no runtime, com ou sem
  esquema no backend; `coding_conteudo_abreviado` recusa "...", "// ...",
  "{ ... }", "… resto do código" (não "...args").
- `coding_esquema_edicoes(n)` gera o mesmo contrato como JSON Schema
  (`anyOf` por operação, `required`, `additionalProperties: false`,
  `maxItems` n). Conferido nesta máquina: o Ollama 0.40 e o llama-server
  (llama.cpp, build de 2026) aplicam essa gramática — mesmo com um exemplo
  no prompt que omite `conteudo`, a saída o inclui. A gramática é ajuda;
  a prova é a validação do runtime.
- Depois do contrato: arquivo lido nesta tarefa, dentro do projeto, fora
  das proibições do pedido (inclusive referência literal no conteúdo),
  `php -l` isolado para PHP, prévia com hash e aprovação explícita.

## 2. Término da geração

Os clientes (`ollama/cliente_*.c`) leem o motivo de parada do backend
(`done_reason` no Ollama; `stop_type`/`stopped_*` no llama-server) e o
número de tokens; `ModeloResposta.fim` é sempre um de `completo`,
`limite_tokens`, `timeout`, `cancelado`, `transporte`, `limite_bytes`,
`resposta_invalida`.

**Truncamento** (`limite_tokens`) não é proposta malformada: etapa
`truncado`, sem gastar recusa; a próxima chamada pede UMA edição (prompt e
`maxItems: 1`) e a estratégia vale para o resto da tarefa; um segundo
corte encerra com causa `truncamento` e orientação. Erros do backend
encerram com causa `modelo_<fim>`.

## 3. Árvore (DAG) da tarefa — `lia-arvore-tarefas/1`

Nó: `id` estável, `pai`, `deps`, `objetivo`, `escopo`, `entradas`/`saidas`
(JSON tipado), `contexto` (caminho, origem, sha256, trecho), `criterios`
(descrição, origem, conferido, ok, como), `estado` (pendente, executando,
bloqueado, falhou, passou), `orcamento` (tentativas, ms, tokens),
`evidencias` (patch: arquivos + hashes antes/depois; validação: comando,
código de saída, arquivos@hash, **alcance**), `tentativas` (hash da
proposta, artefatos de prompt e resposta, resultado).

Regras (`sessiond/arvore.c`, `make teste-arvore`):

- pai e dependências precisam existir; ciclos são recusados, também ao
  carregar um JSON (a retomada descarta árvore inválida, com aviso);
- a mesma proposta (hash) de novo no mesmo nó é "repetição sem progresso";
  orçamento de tentativas finito;
- **um nó só passa** com os próprios critérios conferidos e a **própria**
  última validação aprovada **depois do último patch de toda a sua
  subárvore**; filho que passou nunca promove o pai; dependência que não
  passou bloqueia;
- cada evidência declara o alcance: "validação "teste" passou (código 0)
  com soma.c@1f2e…, depois do patch #4; vale só para esses
  arquivos/hashes e esse comando";
- código, diffs, prompts e logs **nunca** vão no JSON: são artefatos
  endereçados pelo sha256 do conteúdo (`artefatos/<sha256>`, 0600, sem
  sobrescrever); eventos em `eventos.jsonl`.

No ciclo real: `raiz` (o pedido) → `contexto` → `proposta-N` (uma por
ciclo, dependente do contexto) → validação registrada na raiz. O relatório
mostra `Árvore da tarefa: raiz passou|falhou — motivo` e "concluído"
exige que a regra antiga e a da árvore concordem.

**Tentativas isoladas**: cada tentativa guarda os próprios artefatos e
nunca sobrescreve outra; o projeto só muda com a prévia aprovada da
tentativa escolhida. A execução de candidatas alternativas **em paralelo
em worktrees** ainda não existe no runtime — na avaliação, cada execução
roda num clone próprio (`ferramentas/avaliacao/avaliar.py`).

## 4. Contexto

Regras antes do modelo: quem define os símbolos citados, os arquivos de
código citados, os testes existentes deles; quem define o mesmo nome em
**outra linguagem** do que o pedido indica (extensão citada, "PHPUnit",
"pytest", "Unity"...) fica de fora, com nota; `.lia/` (harness e
configuração) e arquivos ocultos nunca entram. Sem nada, o modelo escolhe
até 4 arquivos **da lista do projeto**; a escolha precisa ter relação
verificável com o pedido (palavra de assunto no caminho ou ≥ 2 no
conteúdo), senão a Lia pede o caminho. Limite: relação semântica sem
palavra em comum não é vista; com menos de 2 palavras de assunto, a
escolha é aceita.

## 5. Dados e treino

- `LIA_SESSIOND_TRAJETORIAS=1` (+ licença e proveniência) grava
  `lia-trajetoria/1` em `~/.lia/trajetorias/`: pedido, árvore, artefatos
  referenciados, resultado; o projeto vira hash opaco.
- `ferramentas/treino/lia_treino.py`: validação, rótulos (positivo só de
  proposta aplicada em tarefa concluída com validação depois do último
  patch), anonimização, partição por repositório, exclusão dos conjuntos
  congelados, manifesto reprodutível; `verificar-ambiente`;
  `validar-modelo` (manifesto `lia-modelo/1`).
- Receita de LoRA/QLoRA para máquina compatível: `docs/TREINO.md`.

## 6. Avaliação e promoção

`ferramentas/avaliacao/avaliar.py` roda uma tarefa congelada num clone,
com política de aprovação fixa, e depois a **revisão independente** em
bubblewrap: verificação oculta (que a Lia não vê) e suíte do projeto.
Critério de promoção fixado antes de avaliar:
`docs/avaliacao/2026-11-24-criterio-de-promocao.md`. Manifesto do modelo
vigente: `docs/modelos/padrao-vigente.json`.
