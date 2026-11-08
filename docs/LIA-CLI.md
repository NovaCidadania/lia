# Lia CLI — proposta de arquitetura

**Status:** proposta para discussão (a maior parte ainda não implementada)
**Alvo inicial:** Arch Linux, x86-64  
**Objetivo:** uma interface nativa, rápida e independente do shell, que sirva de entrada para o runtime e para o agente da Lia.

> **Nota:** `lia chat` (seção 2, Etapa D) já tem uma implementação real
> hoje — `lia-chat` (`chat/lia-chat.c`), um binário hospedado separado
> (GLib/GIO, como `lia-sessiond`/`lia-gui`), não o `lia` estático sem
> libc desta proposta. É o primeiro cliente de chat interativo de
> terminal do protocolo (`docs/protocolo.md` §9) — conecta ao
> `lia-sessiond` já em execução, nunca duplica a lógica do daemon. Ver
> `make chat`/`make chat-run`/`make teste-chat` e a seção 7 abaixo. A
> fusão disto num futuro `lia chat` unificado (estático, sem libc) é
> trabalho futuro, não desta etapa.

## 1. Decisão central

A Lia deve ser distribuída como um executável nativo chamado `lia` (`lia.exe` no Windows). Bash, Zsh, sh, PowerShell e CMD não implementam a lógica da Lia: apenas iniciam o executável e entregam os argumentos.

A compatibilidade com shells é diferente da portabilidade entre sistemas operacionais:

- **Mesmo SO, shells diferentes:** o mesmo binário pode ser chamado diretamente por Bash, Zsh, sh, PowerShell ou CMD.
- **SO ou arquitetura diferentes:** é necessário compilar uma versão nativa e implementar a camada de integração correspondente. Um ELF Linux não executa nativamente no Windows.
- **Autocomplete:** scripts pequenos e opcionais podem ser gerados para Bash, Zsh e PowerShell. Eles não participam da execução normal nem contêm a lógica do agente. No CMD, a experiência deve degradar para ajuda e conclusão básica, sem fingir que o mecanismo é igual ao dos outros shells.

A gramática do CLI deve ser única e independente do idioma do shell:

```text
lia [opções globais] <comando> [subcomando] [argumentos]
```

A implementação não deve montar strings de shell para iniciar programas. Deve preservar argumentos como vetor (`argv` no POSIX; linha de comando Unicode construída corretamente para `CreateProcessW` no Windows), definir ambiente e diretório de trabalho separadamente e capturar `stdin`, `stdout`, `stderr`, código de saída e sinais.

## 2. Interface proposta

```text
lia                         abre a conversa interativa se stdin/stdout forem TTY
lia --help                  mostra ajuda
lia --version               mostra versão e alvo de compilação

lia chat                    conversa com o modelo local
lia ask "pergunta"          pergunta pontual
lia run "tarefa"            planeja, executa, observa e verifica uma tarefa

lia doctor                  diagnóstico resumido
lia system inspect          inventário da máquina e do SO
lia system capabilities     CPU, RAM, GPU, armazenamento e dispositivos
lia system permissions      usuário, grupos, permissões e capacidades
lia system processes        processos em execução

lia tools discover          descobre executáveis, built-ins e ferramentas
lia tools list              consulta o inventário
lia tools probe --safe      executa somente testes conhecidos e não destrutivos
lia tools probe --name cat  testa uma ferramenta com um perfil de teste conhecido
lia tools execute -- CMD   executa um programa explícito, sem shell implícito

lia files inspect ARQUIVO   identifica o formato pelos bytes e metadados
lia files read ARQUIVO      lê usando o decodificador adequado
lia files decode ARQUIVO    produz a representação intermediária da Lia

lia model list              lista modelos locais
lia model status            estado do backend de inferência
lia model benchmark         mede desempenho do modelo

lia memory show             consulta a memória persistente
lia memory index            atualiza índices locais
lia memory export           exporta dados selecionados pelo usuário
lia memory sync             compartilha pacotes selecionados e assinados

lia permissions show        exibe a política de permissões
lia permissions explain     explica permissões necessárias para uma ação
lia audit list              consulta o registro de ações
lia undo ID                 tenta reverter uma ação reversível

lia completion bash         gera integração de autocomplete para Bash
lia completion zsh          gera integração de autocomplete para Zsh
lia completion powershell   gera integração de autocomplete para PowerShell
```

`lia` sem argumentos só inicia o modo interativo quando a entrada e a saída são terminais. Em pipe ou automação, deve mostrar ajuda ou exigir um subcomando; nunca pode ficar bloqueado esperando entrada inesperada.

### Opções globais

- `--json`: saída estruturada e estável para outras ferramentas.
- `--no-color`: saída sem sequências ANSI.
- `--quiet` e `--verbose`: controle de diagnóstico.
- `--timeout DURAÇÃO`: limite de execução para operações que o aceitam.
- `--dry-run`: apresenta o plano sem produzir efeitos.
- `--cwd CAMINHO`: define o diretório de trabalho para uma operação.

`--yes` não deve autorizar silenciosamente operações privilegiadas ou destrutivas. Aprovação de privilégios e efeitos irreversíveis deve ter política própria.

## 3. O primeiro recurso da Lia: conhecer a máquina

`lia system inspect` e `lia tools discover` devem ser os primeiros recursos reais do agente. O inventário inicial não pode se limitar a `ls /bin`.

### Fontes Linux

1. Diretórios encontrados em `PATH`, preservando a ordem de resolução.
2. Diretórios convencionais como `/bin`, `/sbin`, `/usr/bin`, `/usr/sbin`, `/usr/local/bin`, diretórios locais do usuário e outros caminhos configurados.
3. Links simbólicos e caminhos canônicos, sem duplicar o mesmo executável.
4. Built-ins, funções, aliases e comandos internos dos shells instalados, registrados separadamente dos executáveis.
5. Metadados do sistema: distribuição, kernel, arquitetura, CPU, memória, armazenamento, mounts, dispositivos, grupos e capacidades.
6. Origem do pacote, quando disponível (por exemplo, `pacman`, `dpkg` ou `rpm`). A consulta ao gerenciador de pacotes é uma fonte auxiliar, não requisito para reconhecer um binário.

O inventário precisa diferenciar um arquivo executável de um comando interno do shell. `cd`, `export` e `alias`, por exemplo, normalmente não são programas que a Lia possa executar por `execve`.

### Registro por ferramenta

Cada item deve guardar, quando aplicável:

- nome e caminho encontrado;
- caminho canônico e informação de link simbólico;
- tipo de arquivo e arquitetura;
- proprietário, grupo e bits de permissão;
- resultado de verificação de acesso pelo usuário atual;
- pacote de origem e versão, quando consultáveis;
- perfil de teste usado, resultado, código de saída, duração e limite aplicado;
- data da verificação e versão do formato do inventário.

Estados de execução sugeridos:

```text
DISCOVERED       encontrado, ainda não testado
ACCESSIBLE       permissões aparentam permitir execução
PROBE_SUCCESS    teste conhecido executado com sucesso
PROBE_FAILED     teste conhecido falhou
PERMISSION_DENIED execução recusada pelo sistema
NEEDS_INPUT      exige parâmetros ou interação
NO_SAFE_PROBE    não existe teste seguro conhecido
```

### Teste real sem danificar a máquina

A Lia deve testar a execução, mas não pode executar indiscriminadamente todo arquivo de `/bin` ou `/usr/bin` sem argumentos. Isso pode bloquear, alterar dados, desligar a máquina ou produzir efeitos irreversíveis.

O processo terá três níveis:

1. **Descoberta estática:** identifica o caminho, tipo e permissões.
2. **Teste de acesso:** verifica permissões efetivas do usuário atual. Esse resultado é apenas indicativo; não prova que o `exec` terá sucesso.
3. **Probe real:** inicia o programa com argumentos inofensivos definidos em um catálogo de probes, com tempo máximo, limite de saída, ambiente controlado e sem elevação de privilégios.

Se não houver probe seguro conhecido, o estado será `NO_SAFE_PROBE`, não “falhou”. Nunca executar um comando desconhecido apenas para descobrir o que acontece. Nunca usar `sudo` no inventário automático.

O teste deve ser feito no contexto do usuário atual. O inventário também registra quais operações podem exigir privilégios, sem tentar obtê-los automaticamente.

## 4. Execução de processos sem depender do shell

Este é um dos principais pontos de desempenho, previsibilidade e segurança da CLI.

- Em Linux/macOS, a Lia chama executáveis por interfaces de processo do SO, com argumentos separados, ambiente e diretório de trabalho explícitos.
- No Windows, usa a API nativa `CreateProcessW`, com caminho do executável definido separadamente e codificação Unicode correta.
- A Lia não usa `system()` ou `popen()` como mecanismo padrão.
- A Lia não concatena entrada do modelo em `sh -c`, `cmd /c` ou PowerShell.
- Pipes, redirecionamento, captura de saída, timeout, cancelamento e código de saída são gerenciados pelo runtime.
- Um shell só é iniciado quando a tarefa pede explicitamente semântica de shell (por exemplo, executar um script Bash). Nesse caso, a escolha do interpretador é explícita e auditada.

Assim, `lia tools execute -- /usr/bin/ls -la /tmp` significa executar aquele programa com aqueles argumentos, e não pedir ao shell que interprete uma string construída pela IA.

## 5. Arquitetura interna da CLI

A CLI é uma camada fina que despacha para módulos internos do mesmo processo. Os subcomandos comuns não devem iniciar processos auxiliares sem necessidade.

```text
Entrada do shell
      |
      v
lia (binário nativo)
      |
      +-- parser de argumentos e modo TTY/pipe
      |
      +-- roteador de comandos
      |
      +-- runtime C
      |     +-- processos e pipes
      |     +-- sistema de arquivos e formatos
      |     +-- inventário e cache
      |     +-- HTTP/JSON para o backend local
      |
      +-- núcleo Assembly
      |     +-- entrada/saída de baixo nível
      |     +-- memória e comparações
      |     +-- rotinas críticas medidas por benchmark
      |
      +-- agente
            +-- planejamento
            +-- política de permissões
            +-- ferramentas
            +-- observação e verificação
```

### Divisão de responsabilidades

**Assembly (L0):** ponto de entrada e primitivas de baixo nível por arquitetura, syscalls Linux no primeiro alvo, operações fundamentais de memória e kernels realmente críticos. Cada rotina exportada terá contrato de registradores, alinhamento, erros e ABI documentados.

**C (L1+):** parser da CLI, estruturas de dados, alocação, descoberta, processos, comunicação HTTP/JSON, cache, decodificadores e integração entre módulos. Implementações Assembly terão testes diferenciais contra versões de referência em C.

**Shells:** autocomplete e integração visual opcional. Nenhuma lógica essencial de descoberta, segurança ou execução deve viver nesses scripts.

### Inicialização rápida

- `lia --help`, `lia --version`, `lia system inspect` e utilitários de arquivo não devem carregar o modelo.
- O backend de inferência só é contatado em comandos que precisam dele.
- Comandos simples devem evitar alocação dinâmica quando possível.
- Inventários são armazenados em cache e atualizados sob demanda ou quando metadados relevantes mudarem.
- Medir cold start, uso máximo de memória, syscalls, latência e throughput separadamente.

## 6. Agente: capacidade ampla com controle verificável

O agente deve poder operar arquivos, processos, configurações, dispositivos e interfaces gráficas, mas não deve executar toda a IA como `root`.

Fluxo obrigatório para ações com efeito:

```text
intenção
  -> plano estruturado
  -> classificação de risco e reversibilidade
  -> checagem de capacidade/permissão
  -> aprovação quando exigida
  -> execução isolada
  -> observação do resultado
  -> verificação pós-condição
  -> auditoria e, quando possível, reversão
```

Recomendação para Linux: processo principal sem privilégios e um auxiliar mínimo (`lia-raiz`) para operações administrativas. O auxiliar expõe uma lista restrita de operações, valida parâmetros independentemente do modelo e registra cada ação. Ele não deve oferecer um shell root genérico.

A elevação com `sudo`/polkit/UAC deve ocorrer apenas para uma ação concreta, com explicação do motivo e aprovação conforme a política configurada. Instruções vindas de arquivos, PDFs, páginas web ou resultados de ferramentas são dados não confiáveis, não autorização para executar comandos.

Cada ferramenta terá metadados: capacidade requerida, escopo de arquivos, efeitos, reversibilidade, timeout, necessidade de confirmação e método de verificação.

## 7. Contexto local e backend de modelo

A Lia é responsável por construir e persistir o contexto. Ollama é inicialmente um backend local de inferência, não o dono da memória da Lia.

- A Lia consulta seu inventário e sua memória em `~/.lia`.
- Seleciona apenas o contexto pertinente à tarefa.
- Envia mensagens e conteúdo estruturado ao modelo local.
- Consome a resposta em streaming e valida chamadas de ferramenta antes de executá-las.
- Registra o que deve persistir de acordo com a política de memória.

Uma interface interna `lia_model_backend` deve permitir trocar Ollama por `llama.cpp` ou pelo futuro motor nativo sem alterar agente, memória e CLI. O backend deve oferecer capacidades explícitas: chat, geração, embeddings, streaming, multimodalidade e lista de modelos. Nem todo modelo oferece todas elas.

O modelo inicial deve ser escolhido por compatibilidade de hardware, contexto, qualidade, quantização e licença. “Open-weight” e “open source” não são necessariamente equivalentes; a licença exata do modelo deve ser verificada antes de redistribuição.

## 8. Armazenamento do inventário e memória

Manter a distinção prevista para `~/.lia`:

```text
~/.lia/                         # permissões 0700
├── LEIA-ME.md
├── humano/
│   ├── config.toml
│   ├── permissoes.toml
│   ├── memoria/
│   ├── diagnosticos/
│   ├── ferramentas/
│   └── auditoria/
└── maquina/
    ├── inventario/
    ├── cache/
    ├── indices/
    ├── estado/
    ├── reversao/
    └── adaptadores/
```

`humano/` contém informações que a pessoa pode ler e editar. `maquina/` contém índices, caches, vetores, estado e formatos binários versionados. Ambos devem ter esquemas e versões explícitos.

O compartilhamento entre instalações deve ser opt-in e seletivo. Inventário de máquinas, caminhos pessoais, nomes de usuário, chaves, tokens, histórico bruto e dados privados não são compartilhados por padrão. Pacotes compartilháveis devem ser assinados, versionados e verificados antes de importação.

## 9. Primeira entrega implementável

Não começar com a árvore inteira de comandos. A primeira versão deve provar a fundação da CLI, do núcleo de I/O e do inventário.

### Etapa A — executável e I/O

1. `lia --version` e `lia --help`.
2. `lia-echo`: stdin para stdout, sem alterar bytes.
3. `lia-cat`: abre arquivo, lê em loop até EOF, escreve stdout e fecha descritor.
4. Tratamento de leitura/escrita parcial, `EINTR`, erros e cancelamento.
5. Testes com arquivo vazio, arquivo grande, pipe, stdin interativo, bytes NUL e dados não UTF-8.

### Etapa B — sistema e ferramentas

1. `lia system inspect --json`.
2. `lia tools discover --json`.
3. `lia tools list`.
4. catálogo inicial de probes seguros para ferramentas selecionadas.
5. cache versionado em `~/.lia/maquina/inventario/` e resumo legível em `~/.lia/humano/diagnosticos/`.

### Etapa C — execução controlada

1. processo filho com `argv` separado;
2. captura independente de stdout e stderr;
3. timeout, cancelamento e limite de saída;
4. teste de execução de uma ferramenta inofensiva;
5. auditoria do resultado.

### Etapa D — modelo local

1. ~~interface abstrata de backend~~ Feito: `sessiond/modelo.h` (`sessiond/modelo_mock.c`/`modelo_ollama.c`).
2. ~~conexão somente com o serviço local configurado~~ Feito: `ollama/cliente_ollama.c` só aceita `127.0.0.1`/`localhost` (com resolução real de `localhost` confirmando loopback — `docs/THREAT-MODEL.md` §5.2).
3. ~~streaming de resposta~~ Feito: NDJSON/chunked em `ollama/cliente_ollama.c`.
4. contexto montado pela Lia — parcial: `sessiond/prompt.c` monta o prompt com o manifesto e o pedido da pessoa; ainda não há memória persistente (`~/.lia/humano/memoria`) incluída no contexto.
5. ~~nenhuma execução automática de ferramenta até existir validação de chamada e política de permissões~~ Feito e é garantia permanente, não só desta etapa: o modelo só propõe (`docs/THREAT-MODEL.md` §5.2); toda execução passa pela validação independente de sempre.

### `lia-chat` — cliente de chat interativo de terminal (implementado)

Primeiro cliente de chat de verdade do protocolo (`docs/protocolo.md` §9),
hoje um binário hospedado separado (`chat/lia-chat.c`, GLib/GIO — como
`lia-sessiond`/`lia-gui`, não o `lia` estático sem libc). Conecta a um
`lia-sessiond` já em execução; nunca inicia o daemon, nunca duplica a
máquina de estados do protocolo (reaproveita `gui/protocolo.c`).

```sh
make chat          # compila build/lia-chat
make chat-run       # compila e abre build/lia-chat (usa ~/.lia/run/session.sock)
make teste-chat     # suíte de integração (make teste-chat), 53 testes
```

### Uso

```sh
lia-chat [--socket CAMINHO] [--backend mock|ollama] [--modelo NOME] [--json] [--help]
```

- `--socket CAMINHO`: caminho do socket do daemon (padrão `~/.lia/run/session.sock`).
- `--backend mock|ollama`: só a suposição LOCAL usada por `/status`/`/modelo`
  (que backend sondar) — **nunca troca o backend do daemon**, que decide o
  seu próprio no início via `LIA_SESSIOND_MODELO` (`docs/protocolo.md` §9).
- `--modelo NOME`: nome do modelo usado nessa sonda local (define `LIA_OLLAMA_MODELO`
  no próprio processo de `lia-chat`, não no daemon).
- `--json`: modo NDJSON cru — lê linhas de protocolo prontas de stdin, escreve
  as respostas do daemon cruas em stdout (scriptável; ver `testes/integration/teste_chat.sh`, PARTE E).

Comandos dentro da sessão (`/help` para a lista completa): `/status`,
`/modelo`, `/backend mock|ollama`, `/arquivos listar`, `/arquivo adicionar
CAMINHO`, `/arquivo remover ÍNDICE`, `/plano`, `/simular`, `/aprovar`,
`/rejeitar`, `/cancelar`, `/limpar`, `/sair`. Qualquer outra linha é
enviada ao modelo como pergunta — exceto quando um plano está pendente,
caso em que a linha é lida como `[s/N]` (padrão: **rejeitar**).

### Testar com o backend mock (padrão, sem Ollama)

```sh
make sessiond && build/lia-sessiond &     # LIA_SESSIOND_MODELO não definido -> mock
make chat && build/lia-chat
lia> MOCK_PROPOR:sistema.diagnosticar
lia> /plano
```

### Testar com Ollama (quando instalado)

```sh
ollama serve &                                        # ou já em execução
LIA_SESSIOND_MODELO=ollama build/lia-sessiond &       # ativa o backend real
build/lia-chat --backend ollama
lia> /modelo                                          # confirma disponibilidade antes de perguntar
lia> como você pode me ajudar aqui?
```

### Códigos de saída

| Código | Significado |
|---|---|
| `0` | saída normal (`/sair` ou fim de entrada — EOF) |
| `1` | não foi possível conectar ao daemon, socket inválido, ou o daemon desconectou |
| `2` | uso inválido (argumento de linha de comando) |
| `3` | erro fatal de protocolo (ex.: `protocolo_incompativel`) |
| `130` | interrompido (Ctrl-C duas vezes em menos de 2s) |

### O que `lia-chat` nunca faz

Nunca chama `system`/`popen`/`fork`/`execve`/`posix_spawn`/`sudo`/`pkexec`
(verificado em `make teste-chat`, G1); nunca interpreta a entrada da
pessoa como shell; nunca aprova um plano por conta própria (padrão é
sempre rejeitar); nunca altera um plano recebido — reenvia exatamente o
`plano_id` que o daemon mandou; nunca segue link simbólico ao anexar
arquivo; nunca lê arquivo além do caminho explicitamente informado.

## 10. Validação obrigatória por arquivo

Nenhum arquivo pode ser commitado sem validação real na máquina de desenvolvimento Arch Linux.

Para cada alteração:

1. conferir diff e formato;
2. compilar o arquivo quando aplicável, com warnings tratados como erro;
3. executar testes unitários e testes de integração relevantes;
4. executar sanitizadores/fuzzers nos módulos de parsing e memória quando disponíveis;
5. executar os testes de CLI em TTY, pipe, EOF, arquivo vazio, erro de permissão e interrupção;
6. medir regressão de performance quando tocar o núcleo;
7. registrar no commit quais comandos foram executados e seus resultados.

O ambiente de CI complementa, mas não substitui, a validação local exigida pelo projeto. Arquivo de documentação também deve passar por verificação de links, blocos de código e consistência com a implementação.

## 11. Critérios de sucesso para a CLI inicial

- O mesmo contrato de comandos funciona em Bash, Zsh e sh no Linux.
- O binário usa stdin/stdout/stderr e exit codes de forma previsível.
- Não exige Python, Node.js ou um shell específico em runtime.
- A descoberta reconhece executáveis e built-ins sem confundi-los.
- Probes automáticos são explicitamente seguros, limitados e não privilegiados.
- Execução de ferramentas não usa concatenação de strings nem shell implícito.
- `lia --help`, `lia --version` e os utilitários básicos não carregam modelo algum.
- Toda ação do agente pode ser auditada; operações privilegiadas passam por política separada.

## 12. Referências técnicas

- Ollama API: https://docs.ollama.com/api
- llama.cpp server: https://github.com/ggml-org/llama.cpp/tree/master/tools/server
- Windows `CreateProcessW`: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw
