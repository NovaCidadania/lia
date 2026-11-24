# ADR 0001 — Coding mínimo: o runtime decide, o modelo propõe

- Data: 2026-11-24
- Estado: aceito
- Contexto detalhado: `docs/ARQUITETURA-CODING-MINIMO.md`

## Contexto

Na avaliação real de 2026-11-24 em cinco repositórios locais, o ciclo de
coding com llama3.2:3b não completou nenhuma tarefa:

- o chat ficou em "coletando dados · 0/0 passos": a tarefa de código era
  mostrada como uma tarefa de passos vazia e a etapa real não aparecia na
  linha de status;
- o modelo era consultado **antes** de qualquer leitura do projeto e
  inventou critérios: `return a + b;` para um parser Python, e
  `sends.txt` (arquivo de dados real que o pedido proibia acessar) como
  critério;
- um caminho citado no pedido era descartado quando o modelo também o
  listava, e um pedido de alteração foi roteado para `arquivo.ler`
  ("não leia .env") e outro para `arquivo.anexar` ("preserve o conteúdo…");
- a consulta ao modelo não tinha prazo: com ~7 tokens/s, a proposta
  ficava minutos em silêncio aparente.

## Decisão

1. O **runtime** é dono do estado da tarefa, da autorização, do contexto,
   da execução, da validação, dos checkpoints e das provas.
2. O contexto mínimo é escolhido **por regras** e lido **antes** de
   qualquer chamada ao modelo; o modelo só escolhe contexto (da lista do
   projeto) quando as regras não acham nada, e sem contexto a tarefa
   encerra com motivo.
3. Critérios vêm do **pedido e da validação**. O modelo pode sugerir
   critérios (opcional); o verificador rejeita os que ampliam, contradizem
   ou violam o pedido, ou citam nomes/arquivos/resultados que não estão no
   pedido nem no código lido.
4. Caminhos em instruções negativas nunca são lidos nem alterados.
5. Pedido de código só vira ferramenta de arquivo direta com conteúdo
   literal; caso incerto gera pergunta.
6. Toda consulta ao modelo tem prazo, progresso visível e cancelamento;
   desconexão encerra com checkpoint.
7. "Concluído" só com diff real, validação depois da última escrita e
   critérios conferidos; o relatório diz a origem de cada proposta (regra
   determinística ou modelo).
8. Não treinamos um LLM do zero. Adaptação (LoRA/QLoRA) de um modelo
   pequeno existente fica para depois de um conjunto de avaliação
   congelado e de trajetórias verificadas — e só é implantada se aumentar
   o sucesso estrito sem aumentar falsos "concluído". LoRA não elimina a
   necessidade de carregar o modelo base.
9. Offline continua sendo o padrão; outro backend é opcional, explícito,
   registrado e sujeito à política de privacidade.

## Consequências

- Menos chamadas ao modelo por tarefa (a de critérios deixou de ser
  obrigatória) e prompts menores e mais relevantes.
- Pedidos que não citam arquivo nem símbolo dependem de uma escolha de
  contexto pelo modelo, ou terminam pedindo o caminho.
- Critérios do pedido são conservadores; a qualidade do comportamento
  continua dependendo dos testes do projeto.
- A qualidade do patch continua sendo do modelo; o runtime torna a falha
  visível e honesta.
