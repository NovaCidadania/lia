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

Medido em 2026-11-26, depois de fixados tarefa e critério. Nenhum ajuste usou o teste; houve **uma única** execução de treino (semente 1,
4000 passos, hiperparâmetros padrão do CLI) e **uma única** avaliação de teste.

### Veredito pré-registrado: **o protótipo AINDA NÃO sabe executar a tarefa de forma confiável.**

Capacidade na tarefa estreita **não declarável**: treinado (0,0 % de ocultos) não é maior que o aleatório (0,0 %) nem que o baseline B0 (14,5 %).
Falsos "concluído" do treinado = 0 (critério parcial atendido, mas só porque o modelo quase nunca propõe uma edição que aplique).

### Teste (300 exemplos, 6 famílias nunca vistas; hash da partição `f990f3d4…`, conjunto `3fda174a…`)

| Condição | ocultos aprovados | edição aplica | formato válido | falsos "concluído" |
|---|---|---|---|---|
| Aleatório (sem treino) | 0,0 % | 0,0 % | 0,0 % | 0 |
| **Treinado do zero** | **0,0 %** | 11,3 % | 94,7 % | **0** |
| Baseline B0 (1ª inversão de operador) | 14,5 % | 97,7 % | 100 % | 2 |
| B1 — referência suplementar (usa a suíte visível), **fora do critério** | 31,2 % | 33,9 % | 100 % | 6 |

B1 foi adicionada depois do pré-registro como referência de teto para uma busca sem aprendizado; não entra no veredito.

### Evidência de treino (perda por token da resposta)

Perda de treino caiu de ~5,5 para 0,0006 (real, medida; `~/.lia/modelo-proprio/run1/treino.jsonl`). A perda de **desenvolvimento** foi mínima
no passo 250 (2,55) e **subiu** até 3,87 no passo 4000: o modelo decorou as 24 famílias de treino e não generaliza para famílias novas.
Com 24 famílias de treino, o sinal é pouco para aprender "ache o defeito" em vez de "lembre a família".

### Checkpoint

`~/.lia/modelo-proprio/run1/ultimo.pt` — sha256 `e5d1e39d162e15325d7b73bb8e6da893e08551798c117e6e5680dad4c51437d2`, 20 135 191 bytes,
1 671 520 parâmetros, passo 4000, semente 1, `hash_dataset` `3fda174a…`, código `lia-neural/1`. Treino ≈ 5 467 s em CPU, RSS ≈ 1,3 GB;
avaliação de teste 241 s, pico 460 MB.

### Integração com a Lia real

`e2e-lia --n 12` contra o `lia-sessiond` real (`LIA_SESSIOND_MODELO=lia_proprio`): 12 × `nao_concluido`, 0 falsos "concluído",
nenhuma escrita sem aprovação. O backend, a identidade do checkpoint e o fluxo de aprovação funcionam; o modelo é que não acerta as edições.

### Limitações

Dados 100 % sintéticos e de 36 famílias; tarefa de uma linha; o conjunto de teste, uma vez visto neste relatório, não é mais cego para
iterações futuras (a próxima rodada precisa de nova semente/famílias); uma só semente de treino; sem varredura de hiperparâmetros.
Próximos passos plausíveis (não executados): mais famílias e mutações por família, regularização/early stopping pela perda de dev,
aumento de dados por renomeação de identificadores — escolhidos só olhando dev.

---

# V2 — localizador neural de defeitos (pré-registro, 2026-11-26, ANTES de qualquer resultado)

## Avaliação do trabalho anterior (esboço "repair_*" iniciado por outro agente, nunca executado de ponta a ponta)

Descartado e substituído, por três motivos verificados na leitura do código:
1. **Vazamento de evidência**: o modelo recebia como entrada `passam_visiveis` (quantos testes visíveis cada candidato satisfaz) e um "portão"
   que só aceitava candidatos que passam nos testes visíveis. Com isso a "acurácia" é a de uma busca por teste (o B1 do V1), não da rede.
   Os testes visíveis eram ainda escolhidos para eliminar todos os outros candidatos, então B1 = 100 % por construção.
2. **Bug de treino**: `label_smoothing` com logits mascarados em -1e9 dava perda ≈ 3,4·10⁶ (visto no log).
3. **Generalização inflada**: 120 "famílias" eram só 6 esqueletos com nome/comentário diferentes; o split "por família" repartia os mesmos esqueletos.

## Hipótese e arquitetura

O V1 falhou ao *reescrever* uma linha byte a byte (11 % das edições aplicavam) e memorizou 24 famílias. O V2 troca a geração livre por
**localização**: a rede (Transformer codificador de bytes, treinada do zero) lê `pedido + arquivo` e **pontua** cada troca atômica possível
(operador, comparador, and/or, constante ±1, min/max, True/False) pelo seu token no arquivo. O patch é reconstruído de forma determinística
(linha exata, única no arquivo). A rede **não vê o resultado de nenhum teste**; só pedido (que traz um caso que falha) e código.
Ela nunca escreve texto livre, nunca declara "concluído", nunca grava. Fora do escopo (função inexistente, nenhum arquivo, mais de um arquivo)
continua sendo decidido por **regra determinística** do adaptador, e isso está rotulado como regra, não como capacidade do modelo.

## Dados

36 famílias do V1 + 35 novas (`familias2.py`); o defeito é injetado por troca atômica gerada (sem lista manual). Partição por família:
**teste = 18 famílias novas** (nunca usadas em treino/dev e nunca olhadas), dev = 8 novas, treino = 36 do V1 + 9 novas.
Mesmos testes visíveis (3) / ocultos (12) do V1; o defeito precisa mudar o comportamento. Semente de teste distinta da de treino/dev.

## Condições (mesmo holdout de teste)

Principal, **sem feedback de teste** (top-1 da condição, aplicado pelo runtime): (a) pesos aleatórios, (b) **treinado**, (c) B0 = primeira troca
candidata em ordem de fonte dentro da função citada.
Suplementar, **com laço do runtime** (até K=5 candidatas na ordem da condição; conclui na primeira que passa a suíte visível): (d) treinado,
(e) aleatório, (f) ordem de fonte (≈ B1). Mostra o quanto a rede reduz tentativas sobre a busca cega; o laço usa teste, então não é prova de saber.

## Métricas

top-1 exato e recall@3 do alvo, edição aplica, **testes ocultos aprovados** (principal), concluído (suíte visível passa), **falso "concluído"**
(visível passa e oculto reprova), tempo, memória, tamanho do checkpoint, perda treino/dev.

## Critério (fixado agora)

Capacidade declarável **só nesta tarefa estreita** se, no teste: ocultos(b) ≥ ocultos(a) + 10 pontos **e** ≥ ocultos(c) + 10 pontos **e** falsos
"concluído" de (b) = 0. Caso contrário: "ainda não sabe". Hiperparâmetros escolhidos apenas por perda/acurácia de **dev**; o teste roda uma vez no fim.
Em nenhum caso isso é "proficiente em programação": é uma tarefa de uma linha, em funções sintéticas curtas.

## Resultados V2 (medidos uma única vez no teste, depois de fixar o critério)

Dados: `rep-s3` (gerador `lia-repair-dados/2`, semente 1, hash do conjunto `ad2f49dd…`; duas gerações independentes deram arquivos idênticos).
Treino: 10 000 exemplos de 245 famílias (36 do V1 + 9 novas + 200 composicionais agregação×filtro×transformação, adicionadas depois de ver que o
dev não melhorava só com 45 famílias — escolha feita olhando dev). Modelo d=128, 5 camadas, 1 253 889 parâmetros, aleatório no início.
Teste: 900 exemplos de famílias nunca vistas (só 14 das 18 famílias aparecem na amostra de 900; não investiguei por quê).

**Checkpoint avaliado** = o melhor em **perda de dev**, que foi o do passo 250 (o treino foi interrompido por mim no passo ~1000, quando já era
claro que a perda de dev só subia): `~/.lia/modelo-proprio/rep-run3/melhor.pt`, sha256 `3f364b07f343ce73cb37f38c364876482f2838888bcdb1257da0b0984609da0e`,
5 037 888 bytes. Avaliação: 68 s, pico 492 MB.

| Condição (teste, n=900) | top-1 exato | recall@3 | ocultos aprovados (top-1) | falsos "concluído" (top-1) | ocultos com laço K=5 | falsos (laço) |
|---|---|---|---|---|---|---|
| Aleatório | 7,2 % | 27,2 % | 8,9 % | 32 | 44,7 % | 71 |
| **Treinado do zero** | **35,3 %** | **60,3 %** | **36,7 %** | **18** | 74,3 % | 29 |
| B0 (ordem de fonte) | 4,1 % | 48,7 % | 6,3 % | 14 | 69,9 % | 32 |

Treinado − aleatório = +27,8 pp (IC95 [24,3; 31,3]); treinado − B0 = +30,3 pp (IC95 [26,9; 33,8]).

### Veredito pré-registrado: `capacidade_declaravel_na_tarefa_estreita = false`

A margem de ≥ 10 pp sobre o aleatório e sobre o B0 foi atendida com folga, **mas o critério exigia falsos "concluído" = 0 e o treinado teve 18**
(a suíte visível de 3 casos deixa passar edições erradas). Pelo critério fixado, **o protótipo não é declarável como capaz nem na tarefa estreita**.
O que os números sustentam: a rede treinada do zero localiza o defeito muito melhor que o acaso e que a heurística de ordem de fonte, mas
**acerta só ~37 % dos casos, e de forma muito desigual por família** (de 79 % em `soma_quadrados`, 71 % em `tamanho_maior` e 66 % em `conta_letra_a` a
0–7 % em `diferenca_abs`, `esta_ordenada`, `media_ponderada`, `desconto_acima_100`, `conta_subidas`). Não generaliza de forma confiável.
Esse número vem de um checkpoint de 250 passos: a perda de dev piora com mais treino (2,14 → 2,38 → 2,95 no run1; 2,55 → 2,99 → 2,89 → 2,76 no run3),
ou seja, mais treino com estes dados memoriza famílias em vez de generalizar.

O laço do runtime (até 5 candidatas, conclui na primeira que passa a suíte visível) sobe os ocultos para 74 %, mas B0 chega a 70 % com o mesmo
laço: **o ganho do laço vem da busca por teste, não da rede**; a contribuição da rede ali é só reduzir tentativas (2,76 vs 3,36 vs 3,91 em média).

### Integração com a Lia real (lia-sessiond, backend `lia_proprio`, 20 primeiros do teste)

9 sucessos, 8 não concluídos, 3 `concluido_falso`; aprovações exigidas em todas as escritas (nenhuma escrita sem aprovação). O backend identifica o
checkpoint (sha256). Isso reforça: ~45 % de sucesso, 15 % de falsos "concluído". **Não usar como coder.**

### Limitações / o que não foi feito
Funções sintéticas curtas, defeito de uma troca de token; escopo (função existe, um arquivo) é regra determinística, não modelo. Uma semente de
treino e um único checkpoint avaliado. O teste agora foi visto, então rodadas futuras exigem novas famílias. Caminhos plausíveis (não executados):
muito mais diversidade de esqueletos (a composicional ajudou pouco no dev), regularização/aumento de dados, mais capacidade, testes visíveis mais
fortes no runtime para reduzir falsos "concluído". Nada disso foi ajustado no teste.
