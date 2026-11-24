# Coding: arquivos, boas práticas, validação, ciclo de coding e Git

Primeira camada da habilidade de codar da Lia: ler e alterar arquivos do
projeto com segurança, em qualquer codificação, e consultar um catálogo
local de boas práticas. Tudo funciona sem MCP, sem internet e sem depender
de um modelo específico: o planejador reconhece os pedidos em português, o
daemon valida e a pessoa aprova toda alteração depois de ver o diff.

## Projeto

O daemon só toca arquivos dentro de uma pasta de projeto:

- `LIA_SESSIOND_PROJETO=/caminho` define a pasta. `chat/rodar_local.sh` usa
  o repositório Git de onde foi chamado (ou a própria pasta fora de um
  repositório); `LIA_PROJETO=/outra/pasta` escolhe outra e
  `LIA_PROJETO=nenhum` desliga as ferramentas de arquivo.
- A raiz é recusada se for `/`, um diretório do sistema (`/etc`, `/usr`,
  `/tmp` inteiro...), a pasta pessoal inteira ou uma pasta de outro usuário.
- Todo caminho é canonicalizado e resolvido através de links simbólicos;
  `..`, caminhos absolutos e links que saiam da raiz são recusados.
- `.git/` nunca é alterado (o estado do Git é do Git). Arquivos especiais
  (dispositivos, FIFOs, sockets) são recusados.

Sem projeto definido, as ferramentas `arquivo.*` nem aparecem para o modelo,
e pedidos de arquivo recebem uma explicação de como definir a pasta.

## Ferramentas

| ferramenta | o que faz | aprovação |
|---|---|---|
| `arquivo.ler` | texto com números de linha (janela de até 2000 linhas); binário vira hexdump | não |
| `arquivo.listar` | arquivos e pastas (recursivo, padrão `*.c`, limite), sem `.git/` | não |
| `arquivo.buscar` | texto ou regex nos arquivos de texto, com arquivo e linha | não |
| `arquivo.info` | tamanho, permissões, codificação, quebra de linha, tipo, sha256 | não |
| `arquivo.criar` | arquivo novo (nunca sobrescreve) | **sim** |
| `arquivo.escrever` | substitui o conteúdo inteiro de um arquivo existente | **sim** |
| `arquivo.editar` | troca um trecho exato; precisa ser único (ou `todas=true`) | **sim** |
| `arquivo.anexar` | acrescenta ao fim | **sim** |
| `arquivo.apagar` | um arquivo, um link ou uma pasta vazia — nunca em massa | **sim** |
| `arquivo.mover` / `arquivo.copiar` | dentro do projeto, nunca sobrescreve o destino | **sim** |
| `arquivo.criar_pasta` | cria os níveis que faltam | **sim** |
| `arquivo.converter` | codificação, quebra de linha (LF/CRLF/CR) ou BOM | **sim** |
| `arquivo.permissoes` | só o bit de execução | **sim** |
| `arquivo.desfazer` | desfaz uma operação anterior pelo `operacao_id` | **sim** |
| `praticas.consultar` | boas práticas relevantes para uma tarefa | não |
| `validar.listar` | comandos de build/teste do projeto e o estado do isolamento | não |
| `validar.executar` | roda build/testes em isolamento (executa código do projeto) | **sim, a cada vez** |
| `git.status` / `git.diff` / `git.log` | consultas Git em isolamento, somente leitura | não |
| `git.commit` | commit local só com os arquivos mostrados; nunca push | **sim** |

### Prévia, aprovação e desfazer

1. Antes de qualquer plano de escrita, o daemon calcula a **prévia**: o diff
   unificado exato do que vai mudar (`projeto/arquivos.c`, `proj_previa`).
   Se a alteração não for possível como pedida — trecho não encontrado ou
   ambíguo (com as linhas onde aparece), arquivo que já existe, pasta
   inexistente, fora do projeto — o pedido é recusado com o motivo real,
   antes de pedir aprovação.
2. O plano enviado ao cliente traz a prévia (`plano.previa` para ferramenta
   isolada, `plano.previas[]` para tarefas). O lia-chat a mostra colorida e
   pergunta "Aprovar este plano? [s/N]". Nada muda antes disso.
3. Na execução, a prévia é recalculada: se o arquivo mudou desde a
   aprovação (outro editor, `git checkout`...), nada é alterado e a resposta
   diz isso.
4. Os originais vão para `~/.lia/projeto/desfazer/<operacao_id>/` e a
   escrita é atômica (temporário + rename), preservando as permissões.
   "desfaça a operação <id>" restaura (também com prévia e aprovação).
5. Toda execução é auditada (mensagem `auditoria` com `aprovacao` e log em
   `~/.lia/humano/auditoria/`, sem conteúdo de arquivo).

### Codificações

O texto é sempre entregue em UTF-8; ao gravar, a codificação, o BOM e a
quebra de linha originais são preservados.

- Detecção: BOM (UTF-8/16/32, GB18030); UTF-16/32 sem BOM pelo padrão de
  zeros; UTF-8 válido / ASCII; binário; e, para textos legados, o detector
  estatístico do Mozilla (`uchardet`, quando presente na compilação) com uma
  heurística própria de reserva (coerência de escrita e caixa das letras).
  A resposta diz a confiança quando não é alta.
- Qualquer codificação conhecida pelo `iconv` do sistema (o glibc traz mais
  de mil, de EBCDIC a Shift_JIS) pode ser forçada na leitura
  (`codificacao`) ou usada na escrita/conversão. Um caractere que não existe
  na codificação de destino é erro — nunca vira `?` silenciosamente.

Limitação honesta: nenhuma detecção automática de codificações legadas sem
BOM é infalível em textos curtos; quando a confiança não é alta, peça para
ler com a codificação certa ("leia notas.txt como latin1").

## Pedidos em português

`sessiond/intencao_arquivos.c` transforma pedidos explícitos num plano de
um passo, sem modelo. Exemplos:

```
leia o arquivo src/main.c                          → arquivo.ler
abra sessiond/agente.c nas linhas 100 a 150         → arquivo.ler (janela)
liste os arquivos .c em sessiond                    → arquivo.listar
procure "TODO" no projeto                           → arquivo.buscar
qual a codificação de docs/protocolo.md?            → arquivo.info
crie src/ola.c com ```c ... ```                     → arquivo.criar
substitua "return 0;" por "return 1;" em src/main.c → arquivo.editar
acrescente "- item" ao final de TODO.md             → arquivo.anexar
apague o arquivo build/lixo.o                       → arquivo.apagar
renomeie src/a.c para src/b.c                       → arquivo.mover
converta legado.txt para UTF-8                      → arquivo.converter
torne deploy.sh executável                          → arquivo.permissoes
desfaça a operação 20261123-204201-a1b2c3           → arquivo.desfazer
quais as boas práticas para tratar erros em C?      → praticas.consultar
```

Textos vêm entre aspas, crases ou num bloco ```; caminhos são palavras com
cara de caminho (`src/main.c`, `Makefile`, `.gitignore`) ou pastas citadas
por nome ("em sessiond"). Quando falta algo, a Lia pergunta ("Qual arquivo
devo apagar?"). Pedidos sem relação com arquivos nunca viram operação de
arquivo (testes em `testes/unit/teste_intencao_arquivos.c`).

## Catálogo de boas práticas

`conhecimento/praticas/praticas.json`: 113 práticas em 22 categorias
(raciocínio estruturado, repositório e contexto, design, funcional, C,
testes, refatoração e código legado, padrões de projeto, Git, shell,
sistemas operacionais, segurança, bancos de dados, infraestrutura,
desempenho, depuração, profissionalismo, projeto, documentação).

- **Redação original.** São princípios gerais escritos com palavras
  próprias; o campo `referencias` aponta onde o tema é discutido em
  profundidade (Clean Code, Refactoring, Code Complete, SICP, Pragmatic
  Programmer, Working Effectively with Legacy Code, Tanenbaum, Date...).
  Nenhum trecho dos livros foi transcrito, e os livros protegidos não são
  baixados nem indexados.
- **Esquema** (`schema_version: 2`): `id`, `versao`, `categoria`,
  `linguagens` (`"*"` ou `"c"`, `"python"`...), `ativacao` (termos),
  `regra`, `passos`, `checagens` (observáveis), `anti_padroes`,
  `severidade` (`obrigatoria`/`recomendada`), `prioridade` (0–100),
  `referencias`. Um arquivo com qualquer erro de esquema é recusado inteiro,
  com a prática e o campo do problema.
- **Recuperação** (`projeto/praticas.c`): determinística, sem embeddings —
  termos de ativação (por radical), categoria, linguagem deduzida do pedido
  e prioridade escolhem no máximo 12 práticas dentro de um orçamento de
  bytes. Conversa sem relação com código recebe nenhuma. Práticas de outra
  linguagem nunca entram.
- **Extensão por projeto**: `<projeto>/.lia/praticas.json` acrescenta
  práticas; o mesmo `id` substitui a do núcleo.
- **Dados, não instruções**: o texto recuperado vai para o modelo delimitado
  como orientação; as regras do repositório, os testes e o código existente
  continuam sendo a referência.

## Testes

```
make teste-arquivos            # biblioteca: confinamento, codificações, diff, escrita, desfazer (79)
make teste-intencao-arquivos   # pedidos em português -> plano (59)
make teste-praticas            # catálogo: esquema, extensão, recuperação (26)
make teste-arquivos-projeto    # daemon + chat de ponta a ponta (39)
```

## Fase 2: validação, ciclo de coding e Git

### Validação (build/testes) — `validar.listar`, `validar.executar`

```
rode os testes / compile o projeto / rode a validação "lint"
quais os comandos de validação do projeto?
```

Comandos nomeados vêm de `<projeto>/.lia/validacao.json`

```json
{"comandos": {"teste": ["make", "teste"], "build": {"argv": ["make"], "timeout_segundos": 300}}}
```

ou, sem ele, do Makefile (`build` = `make`; alvos `teste`/`test`/`check`)
ou do Composer: com `vendor/bin/phpunit`, `teste` = `php vendor/bin/phpunit
--do-not-cache-result` e `sintaxe-php` = `php -l`. Esses dois aceitam
`alvos` (arquivos do projeto acrescentados ao argv, visíveis na prévia):
cada alvo é resolvido dentro do projeto e não pode começar com `-`, estar
em `.git/`, ser arquivo sensível nem pasta. No `validacao.json`, a forma de
objeto aceita `"alvo": true`. Para `php`, código em linha (`-r`, `-a`, `-B`,
`-R`, `-F`, `-E`) continua proibido.
Esse arquivo é **dado não confiável** do repositório: só nomeia comandos.
Quem autoriza é a pessoa, a cada execução, vendo a prévia (executável,
argv, diretório, ambiente, isolamento, limites e o aviso de que o comando
roda código do projeto). A política do daemon vale por cima: shells e
"embrulhos" proibidos, código em linha proibido, executável fora de
`LIA_SESSIOND_VALIDACAO_PERMITIDOS` recusado.

`argv` explícito evita um shell intermediário — mas não impede que `make`
ou um script de teste executem comandos. Por isso **toda** execução roda no
bubblewrap (`projeto/processo.c`): sem rede, sem `/home`, sistema somente
leitura, só o projeto gravável, `.git/` somente leitura, ambiente limpo,
limites de memória/CPU/tempo/saída, cancelamento que encerra a árvore de
processos. Sem bwrap funcional, nada é executado (falha fechada) e a
resposta explica como habilitar. Detalhes e riscos residuais:
`docs/THREAT-MODEL.md` §5.5.

### Git — `git.status`, `git.diff`, `git.log`, `git.commit`

```
git status / quais arquivos alterados no repositório?
mostre o diff de src/main.c / mostre os 5 últimos commits
faça commit de src/main.c com a mensagem "Corrige o cálculo"
```

Consultas sem aprovação (isolamento com o projeto somente leitura). Commit
com aprovação, mostrando ramo, mensagem, os arquivos exatos e o diff de cada
um; sem lista, entram todos os alterados — listados um a um. O que estava
preparado fora deles continua preparado. Ganchos desativados, sem
assinatura, configuração do sistema/usuário desligada; nunca push, reset,
clean, rebase, checkout de arquivos ou reescrita de histórico. Autor:
`LIA_GIT_AUTOR="Nome <email>"` no `rodar_local.sh` (padrão: `user.name`/
`user.email` do repositório).

Memória: o git isolado roda com limite de memória próprio
(`LIA_SESSIOND_GIT_MEMORIA_MB`, padrão 1024, entre 128 e 4096) e com
janelas de mmap e threads limitadas (`core.packedGitWindowSize=32m`,
`core.packedGitLimit=192m`, `core.deltaBaseCacheLimit=48m`,
`core.bigFileThreshold=32m`, `core.preloadIndex=false`, `index.threads=1`
e afins). Sem isso, repositórios com packs grandes falhavam com "packfile
cannot be mapped". O teste cria um pack de ~180 MB e roda com 128 MiB.

### Ciclo de coding

```
corrija o bug na função soma em soma.c
crie src/contador.c com um programa que conta as linhas de um arquivo
retome a tarefa de código codigo-20261124-101010-ab12
```

Etapas, cada uma visível no chat (`progresso` com `codigo.etapa`; a linha
de status do lia-chat mostra `código · <etapa> · ciclo n/3 · <mensagem>`):

0. **contexto** (em segundo plano, sem modelo) — `git status` como
   pré-condição (salvo quando o pedido proíbe o Git: "não rode git", "sem
   git"); se falhar, a tarefa segue e o relatório diz que o estado do Git
   não foi lido. A Lia explora o projeto (sem vendor/, node_modules/ e
   arquivos sensíveis), acha quem DEFINE os símbolos citados
   (`Classe::metodo`, `função`) e escolhe o contexto **por regras**: quem
   define os símbolos, os arquivos de **código** citados que existem e os
   testes que já existem para eles (`leilao-estado.test.ts`,
   `test_core.py`, `*Test.php`). Arquivo de código citado que não existe
   vira "a criar". Arquivo de **dados** citado (`sends.txt`, `.csv`,
   `.log`, dumps) nunca é lido só por ter sido mencionado. Caminhos e
   categorias citados em **instruções negativas** ("não leia .env", "não
   altere migrations/", "não execute campaign.py", "sem tocar em logs,
   contatos, banco, dumps, credenciais, .git") viram proibições: nunca
   lidos, nunca gravados, nunca critério. Só quando as regras não acham
   nada o modelo escolhe até 4 arquivos **da lista do projeto** (conferidos
   contra a lista e as proibições); se ainda assim nada for lido, a tarefa
   **encerra** dizendo que não localizou o código — nunca pede uma proposta
   sem contexto.
1. **critérios** — vêm do **pedido e da validação**, sem modelo: a
   validação (nomeada no pedido, "valide com lento", ou a padrão) passa
   depois da última edição; cada arquivo a criar existe; constantes
   exigidas (`TRANSICAO_INVALIDA`) aparecem no diff; só arquivos de teste
   mudam quando o pedido proíbe mexer em produção; nada proibido foi lido
   ou alterado. Com `LIA_SESSIOND_CODIGO_CRITERIOS_MODELO=1`, o modelo
   **sugere** critérios depois de ver os arquivos lidos e o verificador
   descarta (e lista no relatório) o que viola proibição, cita arquivo não
   pedido ou de dados, inventa resultado (trecho que o pedido não pede, ex.
   `return a + b;`), cita identificador que não está no pedido nem no
   código lido, ou não tem relação com o pedido. Erro do modelo aqui não
   interrompe: seguem os critérios do pedido.
2. **proposta** — o modelo propõe edições, com o contexto lido, as boas
   práticas recuperadas e as proibições no prompt, tudo delimitado como
   dado. O verificador só aceita `criar` (arquivo novo, com `criar_pastas`
   para as pastas que faltam, visíveis na prévia), `editar` (trecho exato e
   único; trecho abreviado com "..." é recusado) e `substituir_funcao` (a
   função inteira reescrita, chaves balanceadas, só a função citada muda)
   de arquivo **lido nesta tarefa**, dentro do projeto e fora das
   proibições; aplica em memória e gera UMA gravação por arquivo, presa ao
   hash lido. Conteúdo proposto que **acrescenta** referência a um caminho
   proibido literal (abrir `sends.txt`, `import campaign`) é recusado antes
   da prévia: a validação rodaria esse código sobre o arquivo real.
   **Receita determinística:** em pedidos sobre SQL injection/bind em PHP,
   antes do modelo a Lia tenta uma regra fixa: na função citada, `$var`
   interpolada na SQL vira `?` e `->query($sql)` passa a receber
   `array($var)`; com pedido de teste, cria um PHPUnit com banco falso (sem
   conexão). A proposta passa pelo mesmo verificador, prévia e aprovação, e
   o relatório diz a origem de **cada** proposta aplicada ("ciclo 1:
   receita determinística…" ou "ciclo 1: modelo local (…), respondeu em X s").
   Cada consulta ao modelo tem prazo (`LIA_SESSIOND_CODIGO_TIMEOUT_MS`,
   padrão 10 min): estourou, a consulta é cancelada e a tarefa encerra com
   o motivo — nunca fica ativa esperando.
4. **aprovação da gravação** — plano com a prévia (diff); nada é gravado
   antes do "s"; arquivo alterado depois da prévia = nada gravado.
5. **validação** — plano com a prévia do comando; outra aprovação. Em PHP,
   `sintaxe-php` nos arquivos tocados e `teste` só nos arquivos de teste.
6. **revisão** — se passou: critérios conferidos (lendo os arquivos) e
   relatório com diff, saída da validação, como desfazer e o checkpoint.
   Se falhou: novo ciclo com a saída do erro (até 3), cada um com nova
   prévia e aprovação.

"Concluído" **só** com diff real, validação que passou **depois** da
última edição e todos os critérios conferidos. Caso contrário o relatório
começa com "NÃO concluído" e diz por quê: recusa, proposta inválida duas
vezes seguidas, mesma falha depois da correção (repetição sem progresso),
limite de ciclos, validação indisponível ou interrompida.

Checkpoints: `~/.lia/projeto/coding/<id>.json` (0600) com pedido,
critérios, hashes dos arquivos lidos, arquivos a criar, operações (para
desfazer), propostas (hash) e o estado da última validação — sem conteúdo
de arquivo nem saída de testes. Gravados a cada etapa (`preparo`,
`contexto` — depois da leitura —, `criterios`, `aprovacao_edicao`,
`validacao`, `correcao`) e no fim: `concluido`, `interrompido` (falha,
prazo do modelo), `cancelado` (a pessoa cancelou) ou `desconectado` (o
cliente caiu). "retome a tarefa de código <id>" relê os arquivos, refaz os
critérios a partir do pedido e continua da validação pendente ou de uma
nova proposta.

Roteamento: pedido de código ("corrija", "ajuste", "implemente", "garanta",
"acrescente um teste"… com caminho, símbolo ou termo de código) só vira
ferramenta de arquivo direta quando é uma operação **literal** (conteúdo
em bloco ```, entre aspas ou "com o conteúdo: …"; mover/copiar/apagar).
"Leia core.py e corrija _iter_log", "altere X; não leia .env" e textos que
descrevem comportamento ("preserve o conteúdo do campo…") vão para o ciclo
de coding — nunca para `arquivo.ler` ou `arquivo.anexar`. Escrita de
arquivo sem conteúdo literal fora desse caso gera uma pergunta de
esclarecimento; nada é gravado.

Limitação honesta: a qualidade das propostas depende do modelo local; um
modelo de 3B erra com frequência trechos de `procurar` (o verificador
recusa e pede de novo, uma vez) e demora minutos para escrever código. A
segurança não depende do modelo: nada grava nem executa sem passar pelo
verificador e pela sua aprovação.

### Perguntas analíticas sobre arquivos

"Leia X e explique por que...", "há SQL injection em...?", "leia os
manifests Docker e explique serviços, volumes e riscos": a Lia lê, mostra
as linhas e responde com **Resposta / Evidência / Achados por regra fixa /
Hipóteses / Como foi montada**. Cada referência `arquivo:linha` do modelo é
conferida contra as janelas lidas (fora delas é descartada, com aviso);
evidência = está escrito nas linhas, hipótese = depende do que não foi lido.
Limite honesto: a Lia confere que a linha citada foi lida, não que ela diz
o que o modelo afirma — confira nas linhas mostradas.

Arquivos sensíveis (`.env*`, `*pgpass*`, chaves, dumps, `config/database.php`,
`wp-config.php`...) não são lidos nem entram em buscas. Em arquivos de
configuração, valores de chaves como senha/token/secret viram «omitido»;
em compose e Dockerfile, **todo** valor de `environment:` (inclusive
entradas comentadas) e de `ENV`/`ARG` é omitido — os nomes ficam.

### Manifests Docker

"Leia compose.yaml e Dockerfile e explique serviços, portas, volumes e
riscos": os manifests **citados** são lidos todos, na ordem, sem repetição
(nome solto como `Dockerfile` vale; caminho que não existe é lido e falha
de verdade, sem inventar). Sem caminho no pedido, a Lia descobre os
manifests (compose*.y*ml, docker-compose*, Dockerfile*, até 3 níveis, 6
arquivos). Nada é executado: nenhum `docker`, nenhum contêiner.

Fatos por regra fixa (`coding_fatos_docker`), cada um com a linha:
serviços, origem (build/imagem), portas, volumes, `depends_on`, redes,
deploy, quantidade de variáveis de ambiente (só nomes; valores omitidos);
chave ausente = "não declarado no manifest". Riscos são inferências a
partir de uma linha (bind gravável do host, `docker.sock`, porta sem IP,
imagem sem versão, rede externa, `deploy.mode` fora do Swarm, volume
compartilhado, nome de variável que parece segredo, ausência de `USER`,
`curl | sh`...). Comentários nunca viram fato (prompt injection em
comentário não muda nada). A **Resposta** é o resumo desses fatos; o texto
do modelo aparece à parte como "não verificado", com as frases que
contradizem os fatos (dependência ou porta inexistente) apontadas.

### Pedidos amplos e pedidos de teste

- "melhore os controllers e models" (sem arquivo, classe ou função): a Lia
  não edita nem pesquisa na web — responde pedindo o escopo e mostra uma
  auditoria de escopo limitado já feita (só leitura, regra fixa: SQL
  montada com variável em controllers/models, arquivo e linha).
- "crie tests/XTest.php com um teste PHPUnit para `Classe::metodo`": é
  ciclo de coding. Trecho citado solto não vira conteúdo do arquivo quando
  o pedido descreve código a gerar (conteúdo literal: bloco ``` ou "com o
  conteúdo: ..."). O verificador recusa arquivo de código trivial (só o
  nome do método, PHP sem `<?php`, teste sem método de teste).
- "não altere o model" / "só o teste": proposta que muda arquivo fora de
  testes é recusada pelo verificador.
- Com `Classe::metodo` no pedido, só os arquivos que definem a classe/método
  são lidos (nomes soltos como `order_by` não puxam o framework).
- Antes de pedir aprovação, PHP proposto passa por `php -l` numa cópia
  fora do projeto, no mesmo isolamento (só analisa, não executa); erro de
  sintaxe volta ao modelo com a mensagem real (`LIA_SESSIOND_LINT_PHP=0`
  desliga).
- No ciclo de correção, "criar" de novo um arquivo que a própria tarefa
  criou vira regravação inteira com diff e nova aprovação.

### Modelo: o que aparece e quanto espera

Toda resposta que usa o modelo diz qual (`Modelo: Ollama · llama3.2:3b —
respondeu em 130 s` ou `falhou em …: motivo`); resultado de regra fixa sai
com origem `deterministica`. Análise e proposta de edição pedem saída
estruturada (esquema JSON; `LIA_SESSIOND_ANALISE_ESQUEMA=0` /
`LIA_SESSIOND_CODIGO_ESQUEMA=0` desligam). Enquanto o modelo trabalha, uma
linha de progresso a cada 15 s (`LIA_SESSIOND_INTERVALO_PROGRESSO_MS`), com
tempo e tokens; "cancelar" interrompe a geração. Se o modelo falhar, a
resposta usa o que a regra fixa conseguiu; sem dados suficientes, diz o que
não foi possível inferir.

### Modelo local via llama.cpp

`LIA_SESSIOND_MODELO=llama_cpp` usa um `llama-server` em loopback
(`LIA_LLAMACPP_PORTA`, `LIA_LLAMACPP_TIMEOUT_MS` até o máximo validado).
Modelos instruct precisam do formato de conversa deles:
`LIA_LLAMACPP_FORMATO=chatml` (Qwen) ou `llama3` (e, para o relatório,
`LIA_LLAMACPP_MODELO_NOME`); sem isso (`cru`, padrão)
o `/completion` só continua o texto. Em máquina sem GPU suportada, suba o
servidor com `--device none -ngl 0`.

### Testes da fase 2

```
make teste-processo        # isolamento real: argv literal, ambiente, rede, limites, timeout, cancelamento (22)
make teste-validacao-git   # validação e Git num projeto C temporário (38)
make teste-coding-unit     # partes puras do ciclo: proibições, contexto, critérios do pedido, filtro, roteamento literal, referência a caminho proibido no conteúdo (130)
make teste-coding          # ciclo completo com modelo mock: rejeições, retomada, Git, chat, contexto antes dos critérios,
                           # critérios absurdos, prazo/cancelar/desconexão, caminhos negados, roteamento (66)
make teste-coding-php      # PHP: receita, modelo, Git, análise, Docker (fatos, citados, injection), pedidos de teste/amplos, php -l, correção, lento/cancelar, .env (71)
```
