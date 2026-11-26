# Auditoria do harness e smoke tests (2026-11-26)

Estado inicial: HEAD `e4da85f` (master = origin/master), árvore limpa, nenhum processo de avaliação nem `llama-server`
ativo. Estado final: alterações **não commitadas** (ordem do usuário). Nenhuma matriz completa foi executada.

## Afirmações do relatório anterior, conferidas no código

| Afirmação | Conferido | Resultado |
|---|---|---|
| `matriz.py` é fail-fast | `matriz.py`, laço principal | **Falso**: erro de harness era registrado e o laço seguia para a próxima combinação (só não imprimia FIM). Corrigido |
| O avaliador aprova só o que solicitou | `protocolo.py` | **Falso**: qualquer `plano` era aprovado pela política, mesmo sem `solicitar` e mesmo com id/hash diferente; resposta de outra pergunta era aceita. Corrigido |
| Proposta de coding não é resposta final | `protocolo.py` | Parcial: só `codigo` truthy; agora também `tipo_resposta == "codigo"`, e o teste cobre coding pendente |
| Falha de harness nunca é resultado de modelo | `matriz.py` | Verdadeiro para contagem (`.harness-erro.json`, `.invalido`, `.descartado` não entram); agora com teste explícito |

## Correções

1. **Protocolo estrito** (`protocolo.py`): `plano` só é aceito se corresponde (id **e** hash) ao plano que este cliente
   solicitou, ou se um ciclo de coding foi aberto (`proposta` com `codigo`). Plano inesperado, de outro id ou de outro
   hash é **rejeitado (preso ao hash recebido)** e encerra como `plano_sem_solicitacao`/`plano_divergente`. Resposta, proposta
   ou progresso de outro `pergunta_id` encerra como `pergunta_divergente`. Os demais desfechos (silêncio, desconexão, `erro`,
   evento desconhecido, plano repetido) já eram terminais de harness.
2. **Runner fail-fast de verdade** (`matriz.py`): o **primeiro** erro de harness aborta, grava o manifesto
   (`abortado_por_harness`), imprime `ABORTADO` e sai com **2**. `--continuar-apos-falha` é modo de **diagnóstico opt-in**: continua,
   manifesto com `modo: diagnostico`, `comparavel: false`, **nunca** imprime `FIM` (sai 1), mesmo sem falha alguma.
   Códigos de saída: 0 completo e comparável; 1 incompleto/diagnóstico; 2 harness; 130 interrompido (SIGINT/SIGTERM,
   com limpeza do avaliador, do `lia-sessiond` e de descendentes em sessão própria — testado para os dois sinais).
3. **Defeito de runtime achado pelo smoke test** (não apenas do harness): o baseline sem modelo **apagou
   `inventario/estoque.py`**. Causa: o critério 2 do enunciado começa com "remover levanta EstoqueInsuficiente…"; o
   planejador de arquivos tratou a palavra `remover` como verbo "apague" e o pedido virou uma operação de arquivo direta.
   Correção geral (`intencao_arquivos.c`): o verbo de apagar/mover/copiar só vale como verbo **de arquivo** quando o objeto
   vem logo depois (um caminho citado ou "arquivo/pasta/diretório" nas 3 palavras seguintes). Teste de regressão escrito
   **antes** da correção (falhou 2 vezes, passou depois), incluindo "remova o ramal em core.py" e os casos legítimos
   (`apague core.py`, `apague o arquivo build/lixo.o`, `exclua a pasta tmp/cache`).
4. **Política de aprovação do avaliador**: `arquivo.apagar` e `arquivo.mover` passam a ser **rejeitados** numa tarefa de
   coding (antes só caminhos fora do escopo eram recusados, então a deleção foi aprovada). Declarado antes de qualquer matriz.
5. **Atribuição da falha** (`atribuir()` em `avaliar.py`, campo `atribuicao` em cada resultado; heurística declarada e igual
   para todas as condições): `sucesso`, `harness`, `ambiente`, `sem_oraculo`, `oraculo_ou_ambiente`, `runtime` (concluído
   sem que o requisito fosse atendido), `backend`, `modelo`, `baseline_sem_modelo`, `indeterminado`. Execução de harness
   inválido jamais é atribuída ao modelo.
6. Versão do avaliador: `avaliar/4` (o hash do avaliador e do binário vai em cada resultado e o runner só reaproveita
   resultado da mesma versão).

Inalterado e reconfirmado: sucesso estrito exige "concluído" + diff + validação depois da última edição + oráculo oculto
aprovado + suíte depois verde + original intocado + nenhuma proibição violada (`avaliar.py`, `sucesso_estrito`); contexto:
"não leia emails.txt" bloqueia `emails.txt` (também com `./`, `..`, `//` e symlink) e não `emails_send.py`.

## Testes automatizados do runtime (gate 1) — reexecutados sobre o diff FINAL (2026-11-26 10:40–10:47)

Os números da primeira versão deste relatório (54 harness, 73 intenção) eram de **antes** da revisão final; foram refeitos depois dela.

| Comando | Resultado |
|---|---|
| `make all` | compila, sem `error`/`warning` (`-Wall -Wextra -Werror`) |
| `make teste-harness-avaliacao` | **66** testes, 0 falhas (protocolo, casos-limite, auditoria de diretório, atribuição, política, limpeza, runner, caso curto real com o daemon real, retenção) |
| `make teste-intencao-arquivos` | **82** testes, 0 falhas |
| `make teste-coding-unit` | 174, 0 falhas |
| `make teste` | 288, 0 falhas |
| demais alvos (17 no total, via script de suítes) | arvore 31, treino 41, agente 106, roteador 125, arquivos 82, arquivos-projeto 39, praticas 26, processo 22, validacao-git 46, sessiond 10, coding 87, coding-php 71, web-resposta 50 — todos 0 falhas |
| `git diff --check` | limpo |

Todos os alvos existem e rodaram; nenhum foi omitido. Nota: "build rc=1" no log do script é só o `grep` sem ocorrência de erro.

### Revisão final: casos-limite que não estavam cobertos (agora com teste)

- **Plano sem `plano_id`/`hash`** durante um ciclo de coding derrubava o avaliador com `KeyError`; agora é `mensagem_malformada`, nada é aprovado.
- **`plano_proposto` sem id/hash**: nenhum `solicitar` é enviado.
- **`pergunta_id` nulo ou ausente** em `proposta`/`resposta`: antes era ignorado (a resposta valia); agora é `pergunta_divergente`
  (o daemon real sempre envia o campo; confirmado pelo teste com o daemon real). Os fixtures antigos que omitiam o campo foram corrigidos.
- **Linha JSON que não é objeto** ou `dados` que não é objeto: `mensagem_malformada` (antes: `TypeError` no cliente).
- **Hash diferente do solicitado**: rejeitado preso ao hash recebido; a `resposta` que vem depois **não** é aceita.
- **Resposta após rejeição pela política** do avaliador (da mesma pergunta): termina normalmente como `resposta`.
- **Intenção de arquivo**: com um caminho citado, só o **caminho** próximo ao verbo conta como objeto — "faça limpar() apagar o arquivo
  temporário" não apaga o arquivo do pedido; pontuação (`core.py,`, `core.py.`, `(core.py`), caixa e espaços múltiplos/tab são tolerados;
  "mova a lógica", "copie os campos", "excluir o registro" não viram operação de arquivo.
- Limitação conhecida (falha segura): `Exclua o arquivo (core.py).` com o caminho **entre parênteses** não extrai o caminho e o daemon pergunta
  qual arquivo apagar, em vez de apagar.

## Smoke tests reais por modelo (gate 2): um combo por condição, clone novo

| Condição | Tarefa | Resultado |
|---|---|---|
| baseline sem modelo | `ind-py-2`, `ind-js-1` | terminal em 2 s, `nao_concluido` (`proposta_invalida`), **sem diff**, original intocado, `FIM 2/2` (antes: apagou um módulo; antes ainda: 40 min parado) |
| Ollama llama3.2:3b (digest `a80c4f17…`) | `ind-py-1`, semente 1, T=0.4 | terminal em 70 s, `nao_concluido` por `criterios` (o runtime recusou concluir) e o **oráculo também reprovou** (não houve falso concluído); suíte antes/depois 0/0; aprovações `codigo_edicao` e `codigo_validacao`; original intocado; nenhum processo restante |
| Qwen2.5-Coder-7B Q5_K_M | — | **BLOQUEADO**: `GET 127.0.0.1:8099/health` sem resposta (código 000) e porta fechada, nas duas verificações (início e fim da revisão). Não foi iniciado por mim; nenhum smoke Qwen foi executado |

Smoke test não mede capacidade: 1 tarefa fácil por condição. Resultados em
`~/.lia/avaliacao-resultados/{smoke2-2026-11-26,smoke3-baseline-2026-11-26}` (smoke 1, anterior à correção do `remover`, ficou em
`smoke-2026-11-26` como evidência do defeito).

### Rastreabilidade dos smoke tests

Cada diretório é uma execução própria; **não** são uma matriz e não se somam:

| Diretório | Conteúdo | Avaliador (sha) | Binário | Observação |
|---|---|---|---|---|
| `smoke-2026-11-26` | baseline, 2 tarefas | `fcd915aa5ee3` | `7cd92da8cd68` | **pré-correção**: contém a evidência do módulo apagado; não é resultado válido |
| `smoke2-2026-11-26` | baseline (2) + Ollama (1) | `2727f7f4be87` (baseline) e `209e3ea6e576` (Ollama) | `190d95cb25f8` | **misto**: duas versões do avaliador no mesmo diretório; o `manifesto.json` contabiliza só o Ollama |
| `smoke3-baseline-2026-11-26` | baseline, 2 tarefas | `209e3ea6e576` | `190d95cb25f8` | consistente com o manifesto; 1 condição, logo não comparável |

`ferramentas/avaliacao/auditar_diretorio.py <dir>` (novo, testado) lê cada JSON, confere versão/sha do avaliador, sha do binário e
manifesto, e grava um `AUDITORIA.json` **novo** ao lado (sha256 de cada resultado, metadados, `misto`, `cobertura_confere_com_manifesto`,
`comparavel`). Foi executado nos três diretórios; **nenhum resultado histórico foi alterado**. Em `smoke2` ele registra
`misto: true`, `cobertura_confere_com_manifesto: false`, `comparavel: false` e lista os dois JSONs de baseline fora do manifesto.
O `comparavel: true` que o manifesto original de um smoke de 1 combinação exibe significa só "executou completo", não "é uma comparação".

### Sucesso estrito, falha por causa

- **Sucesso estrito (0 de 0 medidos):** nenhum smoke teve sucesso; exige concluído + diff no escopo + validação depois da última edição +
  critérios + oráculo oculto + suíte depois verde + original intacto.
- A atribuição `modelo` do smoke Ollama é **heurística** (resultado `nao_concluido` com protocolo íntegro). A trajetória
  (aprovações, critérios reprovados, oráculo reprovado) é coerente com falha do modelo, mas isso não é diagnóstico definitivo sem
  ler o diff proposto. Nos JSONs de `smoke2`, o baseline aparece com `atribuicao: modelo` por ter sido gerado antes de existir a
  atribuição `baseline_sem_modelo`; o `smoke3` tem a atribuição correta.
- Falhas de **harness** observadas ao longo da auditoria: espera de 40 min (corrigida antes), handshake. Falha de **runtime**: o
  `remover` virou apagar arquivo (corrigida). Falha de **ambiente**: servidor do Qwen indisponível. Falha de **modelo**: só o
  `nao_concluido` do Ollama, nesta única tarefa fácil.

## Matriz completa de 54 combinações: **não executada**

Como o smoke do Qwen está bloqueado e a matriz ainda não foi pedida, ela fica **preparada, não rodada**:

```
# com o llama-server do Qwen de pé em 127.0.0.1:8099
make teste-harness-avaliacao
ferramentas/avaliacao/independente-v1.sh ~/.lia/avaliacao-resultados/independente-v1-r2   # diretório NOVO
```

Fail-fast por padrão; `FIM` só com as 54 válidas (`manifesto.json`). Nenhum código, prompt, tarefa, oráculo ou critério deve
mudar durante a matriz. Número de combinações válidas hoje: **0 de 54** (só os smoke tests acima).

## Limitações

- O conjunto `independente-v1` **não é cego** (autoria do mantenedor do runtime): diagnóstico, não promoção.
- O Qwen usado é quantização/fine-tune de terceiros **sem licença declarada**: não é candidato oficial; resultados dele não
  justificam treino nem distribuição.
- Nenhum modelo próprio foi treinado; nenhuma proficiência é declarada com base em testes unitários ou smoke tests.
- A heurística `atribuicao` é declarada e simples (não substitui a leitura dos casos).
- Outros verbos (`mude`, `altere`...) e os caminhos de `mover`/`copiar` com **um** só caminho ainda podem virar esclarecimento
  em vez de código; não observado nas tarefas atuais, não investigado.
