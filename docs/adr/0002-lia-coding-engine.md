# ADR 0002 — Lia Coding Engine: contrato, truncamento, árvore com evidências, dados e promoção

- Data: 2026-11-24
- Estado: aceito
- Antecede: ADR 0001 (o runtime decide, o modelo propõe)
- Detalhes: `docs/LIA-CODING-ENGINE.md`, `docs/TREINO.md`

## Contexto

A avaliação real de 2026-11-24 (0/10) mostrou que a falha dominante era do
protocolo, não só do modelo: o esquema enviado ao backend exigia apenas
`acao` e `caminho`, então `substituir_funcao` sem o código era uma saída
"válida" (7 de 10 tarefas, confirmado pela resposta bruta); respostas
cortadas no limite fixo de 1400 tokens apareciam como "JSON malformado"; e
o runner do Ollama subia com 4096 tokens de contexto, menos que prompt +
proposta, sem a Lia pedir mais.

## Decisão

1. **Contrato por operação** no runtime (`coding_contrato_edicao`) e o
   mesmo contrato como esquema `anyOf` no backend, só com recursos que a
   gramática do llama.cpp (Ollama e llama-server) aplica — conferido nesta
   máquina. A gramática não substitui a validação.
2. **Término da geração** propagado dos backends (`ModeloResposta.fim`).
   Truncamento tem etapa, causa e estratégia próprias (uma edição por
   chamada), nunca vira "JSON malformado". Limite e janela configuráveis
   (`LIA_SESSIOND_CODIGO_MAX_TOKENS`, `LIA_OLLAMA_NUM_CTX`).
3. **Árvore/DAG versionada** por tarefa, com evidências de alcance
   declarado; "concluído" exige a raiz passando pelas regras da árvore
   (validação própria depois do último patch da subárvore). Filho que
   passou não prova o pai. Código e logs só como artefatos por hash.
4. **Toda tarefa encerra com causa** registrada (relatório, checkpoint,
   avaliação).
5. **Trajetórias só com opt-in** e licença/proveniência declaradas; o
   pipeline recusa o resto e nunca usa os conjuntos congelados.
6. **Nenhuma promoção de modelo** sem o critério fixado antes da
   avaliação (`docs/avaliacao/2026-11-24-criterio-de-promocao.md`) e um
   manifesto `lia-modelo/1` válido. O padrão nunca muda em silêncio.
7. Assembly/instruções de máquina não são estratégia de redução de RAM;
   inferência eficiente é medida com quantização, contexto limitado, cache
   KV e paralelismo.

## Consequências

- O modelo não consegue mais devolver uma edição sem código; quando não
  consegue escrever o código dentro do limite, o motivo aparece como
  truncamento, com orientação.
- Propostas tendem a ser mais longas (o código é obrigatório): o limite
  padrão subiu para 2048 tokens e a janela para 8192.
- A execução de candidatas alternativas em worktrees paralelos fica para
  depois; hoje as tentativas são sequenciais e isoladas como artefatos.
- Treinar continua bloqueado por dados e hardware; o caminho até lá está
  executável e testado.
