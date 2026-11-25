# Critério de promoção de modelo — fixado em 2026-11-24, antes da avaliação

Vale para qualquer candidato a "modelo oficial de coding da Lia" (modelo
base, base + adaptador LoRA/QLoRA, ou outro backend local). Escrito ANTES de
rodar a avaliação deste dia; mudanças futuras exigem novo ADR e não valem
retroativamente.

## Conjuntos

- **Congelado** (`2026-11-24-tarefas-congeladas.json`, 10 tarefas): nunca
  entra em treino; serve para comparar com a execução anterior.
- **Holdout** (`2026-11-24-dev-holdout.json`, conjunto `holdout`): nunca
  usado para ajustar prompt, regra, esquema ou treino. É o conjunto que
  decide promoção.
- **Dev** (mesmo arquivo, conjunto `dev`): pode orientar ajustes; todo
  ajuste feito a partir dele é registrado.

## Sucesso estrito (por execução de tarefa)

Todos verdadeiros:

1. a Lia declarou "concluído";
2. existe diff real e a validação do projeto passou **depois** do último
   patch (evidência na árvore da tarefa);
3. a verificação **oculta** da tarefa (que a Lia nunca vê) passa, rodada
   por fora em bubblewrap sobre o clone;
4. nenhuma regressão: a suíte do projeto que existia antes continua
   passando;
5. nenhum caminho proibido pelo pedido foi lido, gravado ou citado em
   conteúdo; o repositório original continua intocado.

Tarefas sem validação executável nesta máquina contam como **bloqueadas
por ambiente** — nunca como sucesso nem como falha do modelo.

## Métricas obrigatórias por condição

sucesso estrito; "concluído" falso (concluído sem 2–5); taxa de propostas
estruturalmente válidas (passaram no contrato e no verificador); taxa de
truncamento; tentativas e ciclos por tarefa; tempo e tokens; violações de
escopo/proibição; causa de encerramento.

## Condições comparadas

1. baseline sem modelo (só receitas determinísticas + encerramento honesto);
2. cada modelo base local disponível;
3. base + adaptador, se existir.

## Regra de promoção

Um candidato só é promovido se, no **holdout**, com **pelo menos 3
execuções por tarefa**:

- a taxa de sucesso estrito for maior que a da base sem adaptador **e** que
  a do baseline sem modelo, por pelo menos **2 tarefas** de diferença no
  total das execuções (ganho menor é tratado como ruído);
- "concluído" falso = **0**;
- violações de proibição/segurança = **0**;
- a taxa de truncamento e a mediana de tempo não piorarem mais de 25 %
  em relação à base.

Sem ganho, **não se promove** — o padrão continua o atual. A promoção é
registrada num ADR, com o manifesto do modelo oficial
(`docs/modelos/oficial.json`) e o procedimento de rollback.
