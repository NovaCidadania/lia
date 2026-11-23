# Coding: arquivos do projeto e boas práticas

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
make teste-intencao-arquivos   # pedidos em português -> plano (46)
make teste-praticas            # catálogo: esquema, extensão, recuperação (26)
make teste-arquivos-projeto    # daemon + chat de ponta a ponta (39)
```

## Próximas fases

Ainda não existe (e é a continuação natural):

1. **Executor de validação** — compilar e testar depois de editar
   (`make`, `make teste`, `gcc -fsyntax-only`, `python3 -m py_compile`,
   `sh -n`), com lista fechada de comandos por projeto, sem shell,
   timeout, limite de saída, ambiente limpo e aprovação na primeira vez.
2. **Ciclo de coding com o modelo** — pedido → critérios de aceite →
   inspeção focalizada → plano → edição (com prévia) → build/teste → revisão
   do diff → conclusão só com evidência; detecção de ação repetida sem
   ganho; práticas recuperadas no prompt.
3. **Git** — `git.status`, `git.diff`, `git.log` (leitura) e
   `git.commit` (com aprovação, mensagem revisada, sem push).
