# Avaliação real do coding mínimo — 2026-11-24

Dez tarefas congeladas (`2026-11-24-tarefas-congeladas.json`, sha256
`097de471e7c28501e3ea31eef2ded44cc218421908ce9de6443ff5dd3cce991b`), uma
precisa e uma aberta por repositório, executadas **depois** das correções da
fase 1 (contexto por regras, critérios do pedido, proibições, prazo,
checkpoints, relatório com origem).

**Resultado: 0/10 de sucesso estrito, 0 "concluído" falso, 10/10
repositórios originais intocados.** Este resultado é de uma execução por
tarefa, num modelo (llama3.2:3b em CPU); não sustenta conclusão sobre
"o modelo mínimo codifica bem" nem o contrário além do que está aqui.

## Como foi executado

- Uma execução por tarefa, em sequência, `lia-sessiond` real (executor real,
  bubblewrap) com `LIA_SESSIOND_MODELO=ollama`, `LIA_OLLAMA_MODELO=llama3.2:3b`,
  `LIA_OLLAMA_TIMEOUT_MS=600000`, `LIA_SESSIOND_CODIGO_TIMEOUT_MS=900000`;
  critérios do modelo desligados (padrão). Binário
  `build/lia-sessiond` sha256 `f3a2663b…751b791` (o mesmo nas dez).
- Cada tarefa num clone temporário (`git clone --no-hardlinks`, só arquivos
  versionados — `.env` e dados reais não existem no clone) em
  `/tmp/lia-aval/<id>/repo`, HOME isolado; commit-base registrado.
- Aprovação simulada, registrada: gravação aprovada se a prévia só toca
  caminhos relativos do projeto (nunca absolutos, `..`, `.meta`, `.env*`);
  validação aprovada só se o argv for o do harness.
- Harness de validação (fora do diff avaliado, em `.lia/`, ignorado no clone):
  - sf: `python3 -m unittest discover -s tests -v`;
  - le: `node node_modules/vitest/vitest.mjs run --config .lia/vitest.unidade.config.mjs`
    (node_modules copiado do original, nada instalado; config sem o
    `globalSetup` que roda `prisma migrate deploy`). Conferido antes, no
    clone limpo e em bubblewrap: 3 arquivos, 13 testes passam;
  - ws: jest não instalado → **bloqueado por ambiente** para validação;
  - sweduc: sem `vendor/` (PHPUnit) → **bloqueado por ambiente** para
    validação (o `php -l` isolado da Lia funciona);
  - mk: Unity sem modo batch isolado disponível (sem editor/dotnet/mono no
    PATH do sandbox) → **sem execução** de testes.
  Nas oito tarefas de ws/sweduc/mk o bloqueio não chegou a importar: nenhuma
  proposta passou do verificador.
- Erro de harness corrigido no meio: `le/leilao_api` é um repositório Git
  próprio, ignorado pelo de fora; a primeira execução de le clonou só os 7
  arquivos do repositório externo (projeto vazio). Esses dois resultados
  foram **invalidados** (guardados à parte) e le foi reexecutado com o clone
  de `le/leilao_api` (commit `cb74f27b9`), mesmo binário, mesmos enunciados.

## Resultados por tarefa

Tempo = parede até a resposta final. Tokens = soma das chamadas ao modelo
(prompt / geração) do log do Ollama. Nenhuma tarefa teve retrabalho humano;
"tentativas" = propostas pedidas ao modelo.

| Tarefa | Commit-base | Tempo | Chamadas | Tokens (prompt/ger.) | Contexto lido (regras ou modelo) | Desfecho |
|---|---|---|---|---|---|---|
| sf-preciso | b56c9222c | 475 s | 3 | 7200 / 1808 | core.py (regra: define `_iter_log`); tests/test_core.py a criar | gravou só o teste (aprovado); validação falhou (SyntaxError); correção recusada (`substituir_funcao` sem conteúdo); 2ª correção cortada em 1398 tokens (JSON inválido) |
| sf-aberto | b56c9222c | 655 s | 2 | 5939 / 1519 | core.py (regra: caminho citado) | 1ª proposta cortada no limite (1396 tokens, 505 s); 2ª: `substituir_funcao` sem conteúdo |
| le-preciso | cb74f27b9 | 200 s | 2 | 4771 / 500 | leilao-estado.ts, http/errors.ts (regra: definem os símbolos), leilao-estado.test.ts (regra: teste existente) | 2× `substituir_funcao` sem conteúdo |
| le-aberto | cb74f27b9 | 355 s | 3 | 7600 / 786 | modelo escolheu 4 rotas HTTP (admin-diagnostico, admin-usuarios, auth, events) — nenhuma é o código de transição do leilão | 2× `substituir_funcao` sem conteúdo |
| ws-preciso | 4fd402150 | 725 s | 2 | 7555 / 1587 | prisma.service.ts (define `PrismaService`), task.controller.ts (citado) | 1ª cortada no limite (1398 tokens); 2ª `substituir_funcao` sem conteúdo |
| ws-aberto | 4fd402150 | 120 s | 3 | 3384 / 243 | modelo: app.controller.spec.ts, app.module.ts, app.service.ts, task.controller.ts | 2× `substituir_funcao` sem conteúdo |
| sweduc-preciso | 8b9b30faa | 610 s | 2 | 6144 / 1591 | Titulo.php e o teste existente TituloTest.php (corretos) **mais** titulo.js, ItemTitulo.php, PagEduTitulo.php (símbolo `Titulo` casou demais) | 1ª: `php -l` recusou o teste (erro de sintaxe); 2ª cortada no limite |
| sweduc-aberto | 8b9b30faa | 530 s | 3 | 5803 / 1762 | modelo: 4 controllers de `backend/` (sem relação com `Titulo`) | `substituir_funcao` sem conteúdo; 2ª cortada no limite |
| mk-preciso | 783d88456 | 410 s | 2 | 6435 / 1038 | ProtocolCodec.cs, ProtocolPrimitives.cs, ProtocolCodecTests.cs (regra) | 2× `substituir_funcao` sem conteúdo |
| mk-aberto | 783d88456 | 145 s | 2 | 4743 / 200 | ProtocolCodec.cs, ProtocolCodecTests.cs (regra) | 2× `substituir_funcao` sem conteúdo |

Comuns às dez: arquivos alterados = nenhum, exceto sf-preciso
(`tests/test_core.py` criado com aprovação); comandos executados = só a
validação de sf-preciso (aprovada; harness, bubblewrap); critérios = só do
pedido e da validação; nenhum caminho proibido lido; status final "NÃO
concluído" com motivo; checkpoint gravado; original intocado (HEAD e
`git status` conferidos antes e depois).

### Revisão independente (sf-preciso, o único diff)

O teste criado pelo modelo, reexecutado fora da Lia em bubblewrap (sem rede,
/home invisível): `SyntaxError: unterminated string literal` (uma quebra de
linha dentro do literal). Além disso, o teste **abre e grava `sends.txt`**
(via `log_send`), arquivo que o pedido proíbe; `core.py` não foi corrigido.
O relatório da Lia mostrava corretamente "validação ✗"; o critério "nada
proibido lido/alterado ✓" estava certo sobre os caminhos tocados, mas não
via a referência no conteúdo — corrigido depois (ver abaixo). Mesmo que
estivesse sintaticamente correto, o diff seria rejeitado na revisão.

### Diagnóstico da falha dominante (fora da contagem)

7 das 10 tarefas terminaram em `substituir_funcao: precisa de "funcao" e
"conteudo"`. Uma reexecução de le-preciso com um proxy que grava a resposta
bruta do Ollama mostrou o que o modelo manda:

```json
{"acao": "substituir_funcao", "caminho": "src/services/leilao-estado.ts",
 "funcao": "validarTransicaoLeilao"}
```

— o nome da função, **sem** o conteúdo; na segunda tentativa,
`"funcao": "validarTransicaoLeilao(...) { ... }"` e um `criar` sobre o
arquivo existente com `...`. O esquema JSON enviado ao Ollama só exige
`acao` e `caminho` por edição, então essa saída é "válida" para a gramática.

## Classificação das falhas

| Classe | Onde | Observação |
|---|---|---|
| modelo | 10/10 | proposta incompleta (sem `conteudo`), abreviada, com erro de sintaxe, ou ignorando a correção pedida (sf-preciso) |
| runtime | 5/10 (sf-preciso, sf-aberto, ws-preciso, sweduc-preciso, sweduc-aberto) | geração cortada no limite fixo de 1400 tokens vira "não devolveu JSON" após 5–8 min; o corte não é identificado como tal nem o modelo é orientado a mandar menos. Esquema permite `substituir_funcao` sem `conteudo` |
| roteamento/contexto | 3/10 (le-aberto, sweduc-aberto: escolha do modelo irrelevante; sweduc-preciso: 3 arquivos a mais) | pedido aberto sem caminho/símbolo depende do modelo para achar o código |
| dependência/ambiente | ws, sweduc (validação), mk (sem execução) | não pesou no resultado: nenhuma proposta chegou à validação |
| validação | 0 | a validação que rodou (sf-preciso) reportou a falha corretamente |
| patch/qualidade | sf-preciso | teste com SyntaxError e acesso a arquivo de dados proibido |
| harness (nosso) | le, 1ª execução | clone do repositório errado; invalidado e refeito |

## Métricas

- Sucesso estrito: **0/10** (preciso 0/5, aberto 0/5).
- "Concluído" falso: **0/10**. Toda tarefa terminou com status explícito,
  motivo, critérios e checkpoint; nenhuma ficou presa (maior: 725 s).
- Tempo total: 4 225 s (~70 min); mediana 443 s por tarefa.
- Modelo: 24 chamadas; ~60 mil tokens de prompt, ~11 mil gerados;
  3,5–8,4 tokens/s de geração; prompts de 2,3–3,6 mil tokens levaram
  14–115 s só para processar.
- Propostas: 21 pedidas, 1 chegou à aprovação (sf-preciso ciclo 1), 20
  recusadas pelo verificador (14 sem `conteudo`, 5 cortadas, 1 `php -l`).
- Aprovações: 2 (uma gravação, uma validação), nenhuma recusada pela
  política.
- Proibições: 0 caminhos proibidos lidos ou gravados; 1 referência proibida
  em conteúdo (sf-preciso) não barrada na época.

## Mudanças feitas depois desta execução

- O verificador recusa conteúdo proposto que **acrescenta** referência
  literal a caminho proibido (`open('sends.txt')`, `import campaign`) antes da
  prévia (`coding_conteudo_cita_proibido`; 4 regressões em
  `teste-coding-unit`). Não altera nenhum resultado acima (a proposta de
  sf-preciso seria recusada um passo antes; continuaria 0/10).
- Harness: clone de subdiretório que é repositório próprio; config do
  vitest em `.lia/` (oculta para a exploração da Lia — na 1ª execução de
  le-aberto o modelo chegou a escolher o arquivo do harness como contexto).

Nada foi ajustado em prompt para fazer um caso passar.

## Próximos passos (genéricos, não ajustados a um caso)

1. Esquema por ação: `substituir_funcao` exige `funcao` **e** `conteudo`;
   `editar` exige `procurar`/`substituir`; `criar` exige `conteudo` (anyOf no
   esquema JSON que vira gramática no Ollama/llama.cpp).
2. Identificar o corte por limite (`done_reason: length`) e reportá-lo como
   tal; pedir de novo com instrução de mandar só a função alterada, em vez
   de gastar outra geração inteira.
3. Inferência do nome da função para métodos sem palavra-chave (C#, Java,
   TypeScript de classe), hoje só `def/function/func/fn`.
4. Seleção de contexto por símbolo: preferir o arquivo da linguagem do
   pedido/caminho citado (sweduc-preciso leu `titulo.js` por `Titulo`).
5. Registrar a resposta bruta do modelo na trajetória (opt-in, fase C do
   plano) — sem ela, a falha dominante só foi diagnosticada com um proxy.
6. Repetir o conjunto congelado com Qwen2.5-Coder-7B (GGUF local, via
   llama.cpp) e com o baseline sem modelo, ≥ 3 execuções por tarefa, antes
   de qualquer conclusão comparativa (fase E).

## Limitações

- Uma execução por tarefa e um modelo; variância não medida.
- Aprovação simulada por política, não por uma pessoa lendo a prévia.
- ws/sweduc/mk não tinham validação executável nesta máquina; se alguma
  proposta tivesse passado do verificador, o resultado seria "bloqueado por
  ambiente", não sucesso.
- No sandbox, o clone de mk aparece com 24 arquivos "alterados" (filtros LFS
  do `.gitattributes` do Unity sem a configuração do usuário); fora do
  sandbox o clone está limpo. Só afeta a mensagem de estado do Git.
- A resposta bruta do modelo foi gravada só na execução de diagnóstico.

## Reprodução

```sh
make && make build/lia-sessiond
ollama pull llama3.2:3b        # já presente nesta máquina
# harness (fora do repositório): avaliar.py <id> <saida.json>, todas.sh <ids...>
./todas.sh sf-preciso sf-aberto le-preciso le-aberto ws-preciso ws-aberto \
           sweduc-preciso sweduc-aberto mk-preciso mk-aberto
# revisão independente de um clone: reexecutar.sh <clone> <argv do teste>
# diagnóstico com resposta bruta: python3 proxy.py proxy-log & LIA_OLLAMA_PORTA=11435 python3 avaliar.py <id> diag.json
```

Os registros completos (eventos, aprovações, diff, log do daemon) ficaram
em `res-<id>.json` no diretório de trabalho da sessão, fora do repositório,
porque contêm trechos do código dos projetos avaliados.

Plano de treino: `docs/ARQUITETURA-CODING-MINIMO.md` §5–§7 (fases A–F). A
fase A está feita; esta execução é a primeira da fase B e mostra que, antes
de adaptar modelo, os itens 1–3 acima são do runtime.
