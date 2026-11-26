# Modelo neural próprio da Lia — protótipo estreito (corrigir defeito de uma linha)

Estado: **protótipo executável**. Não é um modelo de programação geral e nada aqui o declara proficiente.
Este arquivo registra a tarefa e os critérios **antes** de qualquer resultado do conjunto de teste (pré-registro).
Os resultados medidos ficam na seção "Resultados" no fim, preenchida depois.

## Ponto de partida (2026-11-26)

- HEAD `e4da85f`; alterações pré-existentes não commitadas preservadas (harness de avaliação, `intencao_arquivos.c`, relatório de auditoria).
- Máquina: 8 CPUs, 15 GB de RAM, 269 GB livres, sem GPU utilizável. Python 3.14; sem numpy/torch globais.
- Dependência: PyTorch (CPU) num **venv do projeto** (`modelo-proprio/.venv`, ignorado pelo git). Nada global foi alterado.
  PyTorch é só biblioteca de tensores/autograd; **nenhum peso de terceiros** é baixado ou usado.
- Não há `AGENTS.md`/`CLAUDE.md` no repositório; valem `README.md`, `PLANO.md` e as regras de memória do projeto.

## Tarefa escolhida

> **Dada uma função Python pura de poucas linhas com UM defeito injetado (operador, limite de intervalo, constante inicial,
> variável errada), e um pedido em português que diz o que a função deveria fazer e mostra um caso em que ela falha,
> propor UMA edição `procurar → substituir` que a conserta.**

Por que esta tarefa:

1. **Estreita e verificável automaticamente**: a edição é aplicada a um arquivo e executada contra testes; o oráculo é o código correto
   que gerou a tarefa — nenhuma opinião, nenhum LLM de terceiros.
2. **Casa com o contrato que a Lia já tem**: a saída vira `{"edicoes":[{"acao":"editar","caminho":…,"procurar":…,"substituir":…}]}`
   (`sessiond/coding.c`, `coding_contrato_edicao`), que passa pelo verificador, diff, aprovação, escrita e testes existentes.
3. **Dá para separar por famílias** (cada função-modelo é uma família), reduzindo vazamento entre treino e teste.
4. **Cabe numa rede pequena do zero**: prompt+resposta < 512 bytes; o modelo só precisa copiar um trecho e trocar poucos bytes.

Fora do escopo (o modelo deve **pedir contexto ou recusar**, nunca inventar): função citada que não existe no arquivo mostrado,
pedido sem arquivo, e pedidos fora do escopo (apagar arquivos, executar comandos, segredos, funcionalidade nova).

## Contrato do modelo

- Entrada nativa: `pedido` + um único arquivo (`m.py`) com seu conteúdo.
- Saída nativa: exatamente uma de `EDITAR(procurar, substituir)`, `ESCLARECER(texto)`, `RECUSAR(texto)`.
- **O modelo nunca declara "concluído"**; o formato nem tem campo para isso. Só o runtime conclui, depois de aplicar a edição e
  validar. Ele também **nunca escreve arquivos**: só propõe.

## Dados, partições e oráculo

- Gerador determinístico (`modelo-proprio/lia_neural/dados.py`), 36 famílias de função × mutações × nomes × constantes, 100 % sintético
  e escrito por nós (licença própria; nenhum dado de terceiros nem de repositórios do usuário).
- Partições **por família**: treino 24, desenvolvimento 6, teste 6. Pedidos de esclarecimento/recusa também têm modelos de frase
  separados por partição. Hash, semente, versão do gerador e contagens por partição vão no manifesto do dataset.
- Cada exemplo de edição só entra se o defeito **muda o comportamento** (mutante não equivalente), conferido pelos testes ocultos.
- Teste visível (3 casos, visto pelo runtime) ≠ oráculo oculto (12 casos). **Falso "concluído"** = o runtime viu a suíte visível passar
  depois da edição e o oráculo oculto reprovou.

## Condições comparadas (todas no mesmo holdout de teste, mesma semente)

1. **Aleatório**: mesma arquitetura, pesos aleatórios, sem treino.
2. **Treinado do zero**: o checkpoint produzido pelo CLI.
3. **Baseline determinístico simples** (sem aprendizado): inverte o primeiro operador de comparação/aritmético encontrado na função
   citada, o que não conhece nenhuma família.

## Métricas (definidas antes do teste)

perda de treino e de validação (por token da resposta); validade do formato nativo; edições que aplicam (trecho `procurar`
encontrado exatamente uma vez); **taxa de testes ocultos aprovados** (a métrica principal); falsos "concluído"; comportamento correto
em casos de esclarecer/recusar; tempo, memória máxima e tamanho do checkpoint.

## Critério de conclusão (fixado antes do resultado)

- **Infraestrutura pronta** se: treino e inferência executam; pesos treinados recarregam e dão a mesma saída; testes passam;
  resultado comparativo reproduzível; integração opcional sem contornar aprovações.
- **Capacidade declarável só para esta tarefa estreita** se, no holdout de **teste** (famílias nunca vistas), o modelo treinado
  tiver taxa de testes ocultos aprovados **maior** que a do modelo aleatório **e** a do baseline, **e** falsos "concluído" = 0.
  Caso contrário o relatório diz, explicitamente, que o protótipo **ainda não sabe** executar a tarefa de forma confiável.
- Nenhum ajuste de modelo usa o conjunto de teste. Hiperparâmetros são escolhidos olhando só perda/avaliação de **desenvolvimento**.

## Resultados

(preenchidos depois da execução)
