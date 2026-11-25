# Auditoria do Lia Coding Engine (2026-11-24)

Origem: dois relatórios externos (arquiteto de IA e revisão estática) sobre o
diff da missão "Lia Coding Engine". Aqui só consta o que foi **conferido no
código e nos resultados**; o que os relatórios afirmam e não foi conferido
está marcado.

## Balanço honesto

- Resultado de produto: **nenhuma tarefa foi concluída corretamente** pelo
  agente com modelo. O único "sucesso" da matriz (`le-preciso`, llama3.2:3b)
  é falso positivo (achado 1).
- Nenhum modelo foi treinado ou adaptado. Adaptador: **nenhum**.
- A engenharia (protocolo, árvore, dados) existe e é testada, mas **não há
  evidência de ganho de capacidade**. A árvore só pode *reduzir* "concluído"
  (é um portão), nunca criar sucesso; a ablação abaixo mede isso.

## Achados verificados

| # | Achado | Evidência | Estado |
|---|---|---|---|
| 1 | "Sucesso" falso em `le-preciso`: o diff só reformatou o `throw` existente; não tratou a transição desconhecida nem acrescentou o teste pedido. O critério "a alteração inclui `TRANSICAO_INVALIDA`" foi satisfeito por texto que já existia (linha movida) e a tarefa congelada não tem verificação oculta, então o avaliador contou como sucesso | `git_diff` no JSON do resultado | **corrigido** (2, 3 e 4) |
| 2 | `no_diff` aceitava linha "+" que apenas reformatava uma linha "-" | `sessiond/coding.c` `no_diff` | **corrigido**: o literal só conta se as ocorrências em linhas "+" superam as das linhas "-"; teste de regressão com o diff real |
| 3 | Pedido que manda "acrescentar um teste" não exigia arquivo de teste gravado | o diff de `le-preciso` não tem teste e passou | **corrigido**: critério `tocou_teste` (só quando o pedido manda acrescentar/criar/escrever teste); regressão em `teste_coding.c` |
| 4 | Avaliador: `oc is None or ...` contava tarefa sem verificação oculta como sucesso | `avaliar.py` | **corrigido**: classe `sem_oraculo`, `sucesso_estrito = None`, nunca promovível |
| 5 | **Vazamento do holdout**: o código das verificações ocultas ia dentro do JSON de cada resultado (32 de 52) e estava no arquivo de tarefas versionável | resultados + `dev-holdout.json` | **corrigido**: código movido para `~/.lia/avaliacao-oculta/` (0600); o repositório guarda só tipo e sha256; o avaliador confere o hash antes de rodar (falha cedo se ausente/diferente); resultados redigidos (hash em vez de código, saída da verificação ≤ 300 caracteres). Ressalva: os 6 enunciados ocultos já foram vistos por quem desenvolveu esta missão, então o *holdout atual deixou de ser cego*; ver "Próximo" |
| 6 | `ho-sf-2` terminava em `sem_contexto` nas 3 repetições: pedido sem arquivo nem símbolo dependia de o modelo de 3 B escolher um arquivo | resultado `ho-sf-2__*` | **corrigido** com regra, não prompt: busca determinística por palavras de assunto (≥ 2 distintas, no caminho ou conteúdo, mesma linguagem, sem proibidos nem testes) antes de perguntar ao modelo; diz o motivo de cada arquivo lido |
| 7 | Sem ablação árvore × loop simples | — | **ferramenta pronta**: `LIA_SESSIOND_ARVORE_PORTAO=0` / `avaliar.py --sem-portao-arvore`; resultado na seção da matriz |
| 8 | Aprovação de gravação simulada por regras de caminho no avaliador | `avaliar.py` | **verdadeiro e mantido** como limite: é política fixa e reproduzível, não revisão humana; a decisão fica registrada por execução |
| 9 | "Atribuição das mudanças ao Claude Code não demonstrada pelo Git" | 36 itens não commitados | **verdadeiro**: tudo o que está não commitado nesta missão foi feito nesta sessão; a lista completa está em `git status`. Não há commit (ordem do usuário) |
| 10 | Pipeline de treino "não treina" | `docs/TREINO.md` | **já declarado**: é governança de dados + receita; nenhum modelo treinado |

### Segunda rodada (matriz 2: 33 execuções, 0 sucessos estritos)

Com o oráculo corrigido, a matriz 2 (llama3.2:3b com e sem o portão da
árvore, e Qwen2.5-Coder-7B) deu **0/33 sucesso estrito** e 3 "concluído"
falsos (um por condição). Olhando as recusas e os falsos:

| # | Achado | Evidência | Estado |
|---|---|---|---|
| 11 | O localizador de funções não reconhecia `export const f = (..) => expr` (arrow): 18 das recusas ("não achei uma (e só uma) função 'rotuloStatus'") | `le/leilao_api/src/lib/rotulos.ts` | **corrigido** (`regiao_arrow`), com teste |
| 12 | `fim_do_bloco` falhava em TS/JS com regex literal (`/[";\n]/`), template string com `${}` e `return /re/`: 10 recusas em `gerarCsv` | `csv.ts` | **corrigido**, com teste usando o código real |
| 13 | O prompt dava `"caminho":"arquivo lido"` como exemplo e modelos pequenos copiavam o marcador (10 recusas) | prompt de edição | **corrigido** (exemplo com o primeiro arquivo lido de verdade; a recusa lista os arquivos que podem ser editados). É defeito de formato do prompt, não ajuste para um caso |
| 14 | "Concluído" falso em `ho-sf-1` (llama, r3): só criou `tests/test_normalise_phone.py`; `core.py`, citado como alvo ("Em core.py, faça…"), ficou intacto, e nenhum critério exigia isso | resultado `…r3_T0.4` | **corrigido**: critério `tocou_alvo` (arquivo de código citado como alvo precisa ter sido alterado) |
| 15 | "Concluído" falso em `ho-sf-1` (Qwen, r1): alterou `normalise_phone`, mas o ramal continuou entrando; a suíte visível não cobre o requisito | verificação oculta | **não corrigível pelo runtime**: sem critério de aceitação visível, "suíte verde" não prova o requisito. É o ponto central da auditoria externa |
| 16 | Ablação: o portão da árvore não mudou nenhum resultado (1 falso em cada condição de llama, com e sem portão) | `aval3/` | **sem evidência de ganho da árvore** na janela medida; ela fica como registro/auditoria, não como ganho de capacidade |
| 17 | Avaliador mostra diff vazio quando o único arquivo é novo (não rastreado) | `ho-sf-1 …r3` | limite do avaliador (usa `git diff`); o runtime registrou a criação corretamente (`arquivos_novos`) |

Atenção: os achados 11–14 foram descobertos olhando resultados do holdout. Ele
**deixou de ser cego**; as matrizes seguintes valem como medida do código
atual, não como holdout limpo.

Não conferido: a afirmação de que `anyOf` "pode ser ambíguo" entre backends —
o esquema foi testado nos dois backends reais desta máquina (campo ausente é
recusado pela gramática), e o runtime revalida de qualquer forma.

## Critério de promoção: adendo (o original não foi editado)

`2026-11-24-criterio-de-promocao.md` segue valendo. Adendos decorrentes da
auditoria, fixados **antes** de novas execuções:

1. Tarefa sem verificação independente do requisito (`sem_oraculo`) não conta
   para promoção, nem a favor nem contra.
2. "Concluído" falso é falha crítica: um só reprova a promoção.
3. As 10 tarefas congeladas não têm verificação oculta: servem para
   diagnóstico de falhas, não para promoção.
4. Com N pequeno (poucas tarefas × 3 repetições) não há poder estatístico
   para declarar ganho; o relatório mostra contagens, não percentuais.

## Próximo (não feito aqui)

- Novo holdout **cego**: tarefas escritas por quem não desenvolve o runtime,
  com enunciado e verificação guardados fora do repositório.
- Tarefas com critério de aceitação visível no próprio enunciado.
- Só então decidir sobre dados/adaptador.
