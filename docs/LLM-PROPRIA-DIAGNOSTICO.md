# LLM própria da Lia — diagnóstico técnico e plano de P&D (2026-11-26)

Objetivo (do usuário): uma LLM própria que programe **com precisão** (entender pedido e código, localizar, planejar, escrever/alterar, multi-função →
multi-arquivo, autocorrigir, validar por testes), preferindo **poucas tarefas certas** a muitas erradas. Restrições: nada de Llama/Qwen/pesos de terceiros/APIs
de LLM; hardware i7-4702MQ (4 núcleos/8 threads, AVX2), 16 GB RAM, 32 GB swap, GT 750M (sem CUDA útil → tudo em CPU); sem commit/push; os cinco repositórios
de teste **intactos** até haver evidência em tarefas inéditas. Baseline preservado: `~/.lia/modelo-proprio/BASELINE-run3-melhor.pt` (somente leitura, sha256
`3f364b07…da0b0984609da0e`; também em `rep-run3/melhor.pt`).

## 1. O que existe hoje e o que as medições dizem

| | V1 (gerador byte a byte) | V2 (localizador por bytes) |
|---|---|---|
| Saída | linha `procurar→substituir` em bytes | índice de uma troca atômica já enumerada por regra |
| Resultado | 0 % ocultos, 11 % das edições aplicam | 36,7 % ocultos, 18 falsos "concluído" (teste, 900) |
| Falha | memorizou 24 famílias; copiar texto exato é frágil | dev **pior que o acaso** (ver abaixo) |

**Dado novo desta auditoria** (`rep-s4`, perda do acaso = média de ln nº de candidatas): treino 2,86; dev 2,03; teste 2,29. No run4 (bytes, +600 famílias de
expressão) a perda de treino ficou em **2,65 (≈ acaso 2,86)** e a de dev em **2,49 → 2,73 → 3,15 (acima do acaso 2,03)**. Ou seja: **o modelo não aprendeu
nem o treino** e ainda ficou *mais confiante e mais errado* que um sorteio uniforme no dev. O diagnóstico "memorização por falta de diversidade" estava
incompleto: o gargalo é **otimização/representação**, não só dados. (O run3 do passo 250, que gerou os 36,7 %, é um modelo que aprendeu só vieses
simples — p.ex. "um `<` numa comparação com constante costuma ser o defeito" — e funciona nas famílias onde esse viés vale.)

### Gargalos fundamentais (hipóteses → como cada uma será testada)
1. **Representação por bytes**: 298 bytes/exemplo viram 298 posições; a rede gasta camadas aprendendo a ler `"valores"`, números e indentação antes de poder
   relacionar pedido e código. Custo: ~3 s/passo (lote 16). **E1** testa BPE próprio com byte fallback (≈ 2–3× menos posições, atenção ≈ 5–8× mais barata).
2. **Objetivo/cabeça**: o localizador só escolhe entre candidatas que a **regra** enumera; não escreve código, não planeja, não sabe abster-se. Serve ao nível 1
   da escada abaixo, **não** ao objetivo final. Para os níveis 2+ é preciso gerar código (decodificador) com cópia do contexto.
3. **Escala de dados/compute**: ~1 M parâmetros e ~10⁴ exemplos × poucos mil passos em CPU. Para linguagem+código real isso é duas a três ordens de grandeza
   abaixo do que modelos de código pequenos usam; **não dá para prometer** programação geral neste hardware. Dá para uma capacidade estreita e verificada.
4. **Verificação fraca no runtime**: 3 testes visíveis deixam passar edições erradas (falsos "concluído"). Isso é do runtime/protocolo, e é resolvido com
   testes mais fortes e **abstenção**, não com a rede.

### O Transformer atual é apropriado?
Transformer é apropriado; a **instância atual** (codificador de bytes + cabeça de escolha) só é apropriada para o nível 1. Para gerar edições precisaremos de
um decodificador (ou codificador-decodificador) sobre vocabulário de código com mecanismo de cópia (ponteiro) — e isso só vale a pena depois que E1 mostrar
que a representação permite *aprender* (treino abaixo do acaso e dev melhor que o acaso). Não vou aumentar parâmetros/camadas sem esse sinal.

## 2. Escada de capacidades (cada nível com dados e avaliação próprios; só avança se o holdout do nível anterior passar)

| Nível | Tarefa | Dados | Critério para avançar (holdout de famílias inéditas, teste cego novo) |
|---|---|---|---|
| 1 | corrigir operador/constante/expressão (1 troca) | `repair_dados` (+ famílias novas) | ocultos ≥ 70 %, **falsos "concluído" = 0**, > aleatório e > B0 |
| 2 | corrigir função completa (edição de 1–3 linhas) | defeitos compostos, trocas de variável, off-by-one em laço | idem |
| 3 | criar função a partir de especificação | especificação + testes ocultos; saída = função | pass@1 em testes ocultos |
| 4 | modificar funcionalidade existente | (código, pedido de mudança, código esperado) | testes ocultos + testes antigos continuam passando |
| 5 | vários trechos/funções | edições múltiplas ordenadas | idem + nenhuma regressão |
| 6 | vários arquivos | repositórios sintéticos pequenos com testes | idem |
| 7 | repositórios desconhecidos | **os cinco repositórios** (le, mk, ws, sf, sweduc) | só após 1–6 com evidência |

**Abstenção** (reconhecer que não sabe): saída "NÃO SEI" treinada com exemplos sem solução no escopo, e regra de runtime que só deixa o modelo concluir quando a
suíte **forte** passa; métrica obrigatória: *falsos "concluído"* e *taxa de abstenção correta*. Critério de avanço sempre exige falsos "concluído" = 0.

## 3. Regras de avaliação (herdadas e reforçadas)
Particionar por família/estrutura; hashes e sementes por partição; o **teste atual (14/18 famílias já vistas) está "queimado"**: o próximo teste cego precisa de
famílias novas escritas antes de treinar; decisões só por dev; reportar sempre treino, dev e teste, tempo, memória, tamanho do checkpoint; checkpoint e
resultados de cada experimento preservados em `~/.lia/modelo-proprio/`.

## 4. Experimentos
- **E0 (baseline, preservado)**: run3 `melhor.pt`, bytes, 1 253 889 parâmetros.
- **E1 (em execução)**: mesma tarefa, mesmos dados (`rep-s4`), mesmo modelo/hiperparâmetros, **só muda a representação**: BPE próprio (treinado no corpus de
  treino) com byte fallback, contexto 320 em vez de 640. Pergunta: com tokens de código o treino cai abaixo do acaso e o dev passa do acaso?

## 5. Resultado do E1 (decisão só por DEV; teste não tocado nesta etapa)

Mudança única: bytes → BPE próprio (1500 merges pedidos; treinado só no corpus de treino; 256 bytes de base = byte fallback), contexto 640 → 320.
Mesmos dados (`rep-s4`), mesma arquitetura (d=128, 5 camadas, 4 cabeças), mesma semente. Custo: **~0,62 s/passo contra ~1,7 s/passo** (≈ 2,7× mais rápido), pico 1,5 GB.
Também corrigi o treinador: antes ele promovia o checkpoint de menor *perda* de dev (que engana: a rede fica confiante nos erros) e sobrescrevia o estado
final; agora seleciona por **acurácia de dev**, mantém `ultimo.pt` (estado real) e `melhor.pt` (melhor dev) separados. Os testes cobrem BPE, offsets, round-trip e checkpoint.

Avaliação em **dev** (8 famílias inéditas, n=400; `~/.lia/modelo-proprio/dev-rep-e1b-bpe-melhor.pt.json`, `dev-BASELINE-run3-melhor.pt.json`), top-1 do runtime:

| Condição | ocultos aprovados | top-1 exato | recall@3 | falsos "concluído" |
|---|---|---|---|---|
| Aleatório (BPE) | 33,8 % | 28,5 % | 53,0 % | 0 |
| **E1b treinado (BPE, passo 4000)** | **49,8 %** | **47,2 %** | **70,2 %** | 0 |
| B0 (ordem de fonte) | 36,2 % | 36,2 % | 53,2 % | 0 |
| Baseline E0 (run3, bytes, passo 250) | 17,5 % | 14,8 % | 56,2 % | 0 |

E1b − aleatório = +16,0 pp (IC95 [10,8; 21,3]); E1b − B0 = +13,5 pp (IC95 [7,8; 19,0]); E1b − E0 = **+32 pp** no mesmo dev. Curva de dev (acurácia) do E1b: 26 → 24 → 34 → 38 → 34 → 35 → 40 → 42 → 37 → 40 → … → 47 % no passo 4000;
a execução anterior com a mesma configuração chegou a 46,5 % no passo 2500, mas oscilou ±10 pp entre pontos e execuções (dev pequeno: 8 famílias).

Leituras e cuidados:
- **A representação importa**: é o único fator mudado e a acurácia de dev quase dobrou; o treino agora cai abaixo do acaso (1,35 contra 2,86).
- **O E0 do "36,7 %" era sorte de partição**: no dev ele fica *abaixo* do B0 e sem diferença do aleatório. O número de teste daquele modelo não deve ser lido como capacidade.
- **Ainda longe do objetivo**: 50 % de ocultos no nível 1 (uma troca de token) com a suíte visível; precisamos de ≥ 70 % só para avançar de nível. A perda de dev (4,3) é pior que o acaso: o modelo é confiante e errado, e portanto **não pode decidir sozinho "concluído"** nem "não sei" por probabilidade — a abstenção precisa de calibração (próximo passo).
- Nada foi avaliado no teste; o teste atual está queimado e será substituído por um teste cego novo antes de qualquer afirmação de generalização.

## 6. Próximos experimentos (ordem)
1. **E2 – calibração/abstenção**: classe "nenhuma troca resolve" treinada com exemplos fora do escopo e perda ponderada; medir falsos "concluído" e abstenção correta no dev.
2. **E3 – mais variedade estrutural + teste cego novo**: famílias novas escritas ANTES de treinar (e um teste cego com famílias nunca vistas); comparar com E1b.
3. **E4 – nível 2 (defeitos compostos/variável errada)** com saída por cópia (ponteiro) para um decodificador pequeno; só se E2/E3 mostrarem que a representação sustenta.
