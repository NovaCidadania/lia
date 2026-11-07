# PLANO — Lia

Documento vivo. Define a visão, as decisões técnicas, as fases e os critérios de conclusão do projeto. Decisões marcadas **[aberta]** ainda precisam ser tomadas.

---

## 1. Visão

Uma IA **local**, **altamente performática**, **portável** e **agêntica**, construída de baixo para cima sobre as duas funções básicas de qualquer IA: **ler** e **interpretar** dados.

- Lê **qualquer** arquivo do sistema a partir dos bytes, com um decodificador próprio por formato.
- Opera o computador por meio de capacidades explícitas: arquivos, processos, configuração, entrada/saída e hardware, sempre com consentimento, escopo mínimo e auditoria.
- Oferece três interfaces coerentes: CLI para automação, conversa por texto/voz e GUI nativa para inspeção, aprovação e acompanhamento das ações.
- Mantém contexto próprio em `~/.lia`, separando dados para humanos de dados para máquina.
- Treina localmente e compartilha conhecimento com outras instalações somente após aprovação e com pacotes verificáveis.

### 1.1 Princípios de produto

1. **Local por padrão:** dados, memória e inferência ficam no dispositivo; rede é opt-in e visível.
2. **Capacidade, não acesso irrestrito:** cada ferramenta declara permissões, impacto, reversibilidade e pré-condições.
3. **Humano no circuito:** ações destrutivas, externas, privilegiadas ou irreversíveis exigem confirmação clara, salvo uma política previamente configurada pelo usuário.
4. **Falha segura:** ausência de autorização, modelo, driver ou backend deve degradar a função sem elevar privilégios nem ocultar o erro.
5. **Portabilidade por contrato:** interfaces de alto nível não dependem de Linux; backends traduzem para POSIX, Windows, macOS e dispositivos móveis.
6. **Medir antes de otimizar:** Assembly é reservado para caminhos críticos demonstrados por benchmark e cobertos por uma implementação de referência.

## 2. Decisões de base

### 2.1 Linguagens

| Camada | Linguagem | Motivo |
|---|---|---|
| Núcleo (L0) | **Assembly** (linguagem principal) | Controle total: syscalls diretas, sem libc, SIMD explícito. |
| Runtime e cola (L1+) | **C** (C17, `-ffreestanding` onde possível) | Portabilidade e produtividade onde Assembly não traz ganho mensurável. |
| Scripts de build/teste | Shell / Make | Mínimo de dependências. |

**Regras:**

1. **Toda rotina em Assembly tem uma implementação de referência em C.** A versão C é o "oráculo" dos testes: mesma entrada → mesma saída. Isso também dá um caminho de fallback para arquiteturas ainda sem port.
2. **Assembly onde o ganho é medido.** Rotinas críticas (I/O, memória, kernels de matriz, parsing de bytes) começam em ASM. O resto começa em C e migra para ASM quando um benchmark justificar.
3. **ABI interna única e documentada** (`docs/abi.md`): convenção de chamada (System V AMD64 no Linux x86-64, AAPCS64 no AArch64), registradores preservados, alinhamento, códigos de erro.
4. **Sem libc no núcleo.** Ponto de entrada próprio (`_start`), syscalls via `syscall`/`svc`.

### 2.2 Toolchain

- **Assembler:** GNU `as` (sintaxe Intel via `.intel_syntax noprefix` no x86-64). Já presente no sistema, suporta x86-64 e AArch64 com a mesma toolchain. **[aberta]** avaliar NASM/FASM para x86-64 se a sintaxe GAS atrapalhar.
- **Compilador C:** GCC (Clang como segunda opção para validar portabilidade).
- **Linker:** `ld` direto para binários do núcleo (sem `crt0`).
- **Build:** `make`, sem geradores.
- **Arquitetura de partida:** Linux x86-64.

### 2.3 Modelo inicial

- **Llama open source pré-treinado**, servido pelo **Ollama** em `127.0.0.1:11434`.
- Cliente HTTP mínimo próprio em C (socket + HTTP/1.1 + JSON streaming) — sem libcurl.
- Candidatos: Llama 3.x 8B (padrão), Llama 3.2 1B/3B (máquinas modestas). **[aberta]** quantização padrão (Q4_K_M vs Q8_0).
- Em paralelo: motor de inferência próprio lendo **GGUF** diretamente, com kernels em Assembly (fase 6).

---

## 3. Arquitetura em camadas

### L0 — Núcleo em Assembly

Primitivas fundamentais, cada uma um arquivo `.S`, sem libc:

| Primitiva | Syscall Linux x86-64 | Observação |
|---|---|---|
| `lia_open` | `openat` (257) | flags, modo |
| `lia_read` | `read` (0) | trata `EINTR`, leitura parcial |
| `lia_write` | `write` (1) | laço até escrever tudo |
| `lia_close` | `close` (3) | |
| `lia_lseek` | `lseek` (8) | |
| `lia_fstat` | `fstat` (5) / `statx` (332) | tamanho, tipo, permissões |
| `lia_mmap` / `lia_munmap` | 9 / 11 | leitura de arquivos grandes sem cópia |
| `lia_exit` | `exit_group` (231) | |
| `lia_getdents` | `getdents64` (217) | listar diretórios |
| `lia_ioctl` | `ioctl` (16) | dispositivos |

Conceitos implementados explicitamente:

- **stdin / stdout / stderr** como descritores 0/1/2.
- **Laço de leitura** com buffer fixo (ex.: 64 KiB alinhado a página) até **EOF** (`read` retorna 0).
- **Erros** como valores negativos `-errno`, convertidos em códigos Lia.
- Rotinas de memória: `lia_memcpy`, `lia_memset`, `lia_memcmp`, `lia_memchr` (escalar → SSE2 → AVX2 → AVX-512, escolhidas em tempo de execução via `cpuid`).

**Primeiros programas (prova do núcleo):**

1. `lia-echo` — stdin → stdout.
2. `lia-cat` — abre arquivos da linha de comando, laço `read`/`write` até EOF, `close`.
3. `lia-hex` — despejo hexadecimal + ASCII (estilo `xxd`).
4. `lia-tipo` — identifica o tipo do arquivo pelos **bytes mágicos**, não pela extensão.

### L1 — Runtime em C

- Alocador próprio (arena / bump allocator sobre `mmap`).
- Strings com tamanho explícito (sem dependência de `\0`), UTF-8 validado.
- Estruturas: vetor dinâmico, hash map, árvore de prefixos.
- Formatação de saída mínima (sem `printf` da libc).
- Leitor de configuração (TOML subset) para `~/.lia/humano/`.

### L2 — Formatos (decodificadores puros)

Objetivo: **a entrada para o modelo deve ser o conteúdo correto, não uma aproximação.** Cada formato tem um decodificador próprio, escrito a partir da especificação, que produz uma **representação intermediária Lia (RIL)**:

```
RIL = { tipo, metadados, blocos[] }
bloco = texto | tabela | imagem(pixels) | áudio(amostras) | estrutura(árvore) | binário(região)
```

Ordem de implementação proposta:

| Ordem | Formato | Desafio principal |
|---|---|---|
| 1 | Texto: ASCII, UTF-8, UTF-16, Latin-1 | detecção de codificação, quebras de linha |
| 2 | ELF | cabeçalhos, seções, símbolos — Lia entende os próprios binários |
| 3 | DEFLATE / zlib / gzip | base para PDF, PNG, ZIP, DOCX |
| 4 | ZIP | base para DOCX, XLSX, ODT, JAR |
| 5 | **PDF** | objetos, xref, streams, fontes, mapeamento CMap → Unicode, ordem de leitura, tabelas |
| 6 | PNG, BMP | pixels brutos |
| 7 | JPEG | DCT, Huffman |
| 8 | WAV / PCM | amostras |
| 9 | JSON, XML, HTML, CSV | estrutura |
| 10 | DOCX, XLSX, ODT | ZIP + XML |
| 11 | SQLite | páginas B-tree |

Regras:
- Decodificador **não executa** nada do arquivo (sem JavaScript de PDF, sem macros).
- Todo decodificador passa por **fuzzing** (entradas malformadas não podem travar nem corromper memória).
- Formato desconhecido → bloco `binário` com análise estrutural (entropia, strings, padrões), nunca erro silencioso.

### L3 — Inferência

- **Fase inicial:** cliente Ollama em C. A RIL é serializada no prompt de acordo com o tipo (texto como texto, tabelas como tabelas, imagens para modelos multimodais).
- **Fase própria:** carregador GGUF via `mmap`, tokenizador próprio, kernels de matmul/atenção/RMSNorm/RoPE em Assembly (AVX2, AVX-512, NEON), quantização Q4/Q8. Comparação constante contra o Ollama/llama.cpp (tokens/s e saída idêntica).
- **Contexto próprio:** o Ollama conectado à Lia recebe contexto montado pela Lia a partir de `~/.lia` (memória, estado da máquina, histórico relevante) — ver seção 4.

### L4 — Agente e interfaces humano–máquina

A Lia age no computador por meio de **ferramentas** com interface uniforme (nome, parâmetros, efeito, reversibilidade).

| Domínio | Linux (1ª plataforma) | Windows / macOS / móvel |
|---|---|---|
| Comandos do SO | `fork`/`execve`, pipes, captura de saída | `CreateProcess` / `posix_spawn` |
| Arquivos e configuração | núcleo L0 + decodificadores L2 | idem |
| Teclado e mouse (saída) | `/dev/uinput` | `SendInput` / CGEvent |
| Teclado e mouse (entrada) | `/dev/input/event*` (evdev) | Raw Input / IOHID |
| Tela | DRM/KMS, X11 (`XGetImage`), Wayland (portal/`wlr-screencopy`) | DXGI / ScreenCaptureKit |
| Privilégios | polkit, capabilities (`CAP_*`), `sudo` somente quando autorizado | UAC, Authorization Services, APIs equivalentes |
| Serviços | systemd (D-Bus) | SCM / launchd |
| Hardware | `/sys`, `/proc`, PCI config space, USB (`usbfs`), I²C/SMBus, sensores (hwmon), `ioctl`, MSR (`/dev/cpu/*/msr`) | WMI, IOKit |

**Privilégios — objetivo: executar o necessário com a menor autoridade possível.**

Root/Admin não é um estado permanente da Lia e nunca é obtido por armazenamento de senha, alteração automática de `sudoers`, bypass de autenticação ou exploração de vulnerabilidades. A elevação, quando indispensável, usa a API oficial do sistema, mostra o motivo ao usuário, registra a decisão e limita o processo auxiliar à operação solicitada.

- A ferramenta declara `leitura`, `usuario`, `admin` ou `hardware` antes da execução.
- O modo padrão é simulação/planejamento; o usuário aprova a ação concreta e seus efeitos.
- Execução usa `execve`/APIs nativas, sem shell implícito; shell só é possível em comando explicitamente solicitado.
- Toda ação com efeito é registrada antes e depois, incluindo ferramenta, argumentos normalizados, identidade, autorização, resultado e reversão disponível.
- O inventário continua sem privilégios: probes desconhecidos não são executados automaticamente.

Controles fundamentais, implementados antes de qualquer ferramenta destrutiva:

1. **Níveis de poder** configuráveis em `~/.lia/humano/permissoes.toml`: `leitura` → `usuario` → `admin` → `hardware`. O usuário define até onde a Lia vai sem perguntar.
2. **Processo auxiliar privilegiado sob demanda**: o processo principal roda como usuário; uma ação administrativa passa por um canal local autenticado para um auxiliar pequeno e auditável, que termina ao concluir a operação.
3. **Registro de auditoria** append-only de toda ação com efeito (comando, arquivo alterado, antes/depois), legível em `~/.lia/humano/auditoria/`.
4. **Reversão**: antes de alterar arquivo de configuração, cópia em `~/.lia/maquina/reversao/`; comando `lia desfazer`.
5. **Modo simulação** (`--simular`): mostra o que seria feito sem executar.

### L5 — Treinamento local

- Ajuste fino por **LoRA/QLoRA** sobre o modelo base, com dados da própria máquina (escolhidos pelo usuário).
- Adaptadores salvos em `~/.lia/maquina/adaptadores/`.
- Inicialmente via ferramentas existentes; depois, backward pass próprio reaproveitando os kernels de L3.
- **[aberta]** uso de GPU: Vulkan compute (portável) vs CUDA/ROCm.

### L6 — Rede Lia (compartilhamento entre instalações)

- Compartilhar **adaptadores, índices e ferramentas**, não dados brutos do usuário.
- Descoberta em rede local (mDNS) e pares remotos explícitos.
- Pacotes assinados (Ed25519) e verificados antes de aplicar.
- **[aberta]** protocolo: próprio sobre TCP/QUIC vs existente.

---

## 4. Contexto próprio: `~/.lia`

Distinção rígida: **`humano/`** é para pessoas lerem e editarem; **`maquina/`** é formato próprio da Lia, otimizado para velocidade, e não deve ser editado à mão.

```
~/.lia/
├── LEIA-ME.md                  # explica a estrutura para o usuário
├── humano/
│   ├── config.toml             # configuração geral
│   ├── permissoes.toml         # níveis de poder
│   ├── memoria/                # o que a Lia lembra, em Markdown, um arquivo por tema
│   │   ├── usuario.md
│   │   ├── maquina.md          # resumo legível do hardware/SO
│   │   └── projetos/*.md
│   ├── auditoria/AAAA-MM-DD.log
│   ├── conversas/              # histórico legível
│   └── ferramentas/            # ferramentas definidas pelo usuário
└── maquina/
    ├── VERSAO                  # versão do layout binário
    ├── indice/                 # índice dos arquivos do sistema (hash, tipo, RIL)
    ├── vetores/                # embeddings
    ├── cache/                  # RIL decodificada, cache KV
    ├── estado/                 # estado interno do agente
    ├── adaptadores/            # pesos LoRA
    ├── reversao/               # cópias antes de alterações
    └── rede/                   # chaves, pares, pacotes recebidos
```

Regras:
- Arquivos em `humano/` são a **fonte da verdade** para preferências e memória; `maquina/` pode ser reconstruído a partir deles + do sistema.
- Todo formato binário em `maquina/` tem cabeçalho com assinatura mágica `LIA\0`, versão e checksum — e um decodificador em L2 (a Lia lê os próprios formatos como lê qualquer outro).
- Permissões: `~/.lia` com modo `0700`.
- Segredos, tokens e credenciais não são armazenados em texto em `~/.lia`; usar o keychain/credential store do sistema, com referência opaca na configuração.
- A memória tem ciclo de vida explícito: inspeção, exportação, correção e exclusão pelo usuário.

## 4.1 Interface humano–máquina

### CLI

A CLI é a interface primária para automação e depuração, com comandos estáveis e composáveis:

```text
lia chat [--modelo X]
lia executar <tarefa> [--simular|--aprovar]
lia ferramentas listar|inspecionar|autorizar
lia sistema diagnosticar|recursos|dispositivos
lia arquivos ler|buscar|alterar|desfazer
lia memoria consultar|editar|exportar|apagar
lia auditoria listar|verificar|exportar
lia interface iniciar              # abre a GUI local
```

Saídas suportam texto humano, JSON estável (`--json`) e eventos de streaming. Cada comando tem `--ajuda`, código de saída documentado, cancelamento e limite de tempo.

### GUI

A GUI deve ser uma camada fina sobre o mesmo protocolo local da CLI, sem lógica duplicada. Ela terá:

- conversa e streaming de eventos;
- painel de plano, ferramentas, permissões e confirmações;
- visualização de arquivos por representação intermediária, não apenas texto;
- estado de execução, consumo de CPU/RAM/GPU, cancelamento e rollback;
- histórico e auditoria pesquisáveis;
- modo acessível, navegação por teclado e renderização adaptada a telas pequenas.

O transporte inicial será um socket local autenticado, com mensagens versionadas. A GUI não receberá acesso privilegiado direto; somente o daemon de sessão poderá solicitar capacidades autorizadas.

Especificação completa do protocolo: [docs/protocolo.md](docs/protocolo.md).

### API de ferramentas

Cada ferramenta publica um manifesto com nome, versão, parâmetros tipados, capacidades requeridas, classificação de risco, efeitos, idempotência, timeout, limites e estratégia de reversão. O modelo escolhe ferramentas, mas o executor valida o manifesto e a política independentemente.

Especificação completa do manifesto, com exemplos das ferramentas já implementadas: [docs/manifesto.md](docs/manifesto.md).

---

## 5. Fases

| Fase | Entrega | Critério de conclusão |
|---|---|---|
| **0 — Fundação** | Repositório, `make`, ABI documentada, CI (build + testes) | `make && make teste` passa limpo |
| **0.5 — Contratos e CLI** | CLI, JSON de eventos, manifestos de ferramentas, simulação, confirmação e auditoria | comandos documentados, testes de contrato e nenhuma ação externa sem autorização |
| **1 — Núcleo de I/O** | Primitivas L0 + `lia-echo`, `lia-cat`, `lia-hex`, `lia-tipo` em Assembly | Saída byte-a-byte idêntica a `cat`/`xxd`; vazão medida contra coreutils |
| **2 — Runtime** | Alocador, strings, estruturas, config TOML | Testes contra oráculos em C; zero vazamento sob valgrind |
| **3 — Ponte com o modelo** | Cliente Ollama em C, `lia pergunta "..."` | Resposta em streaming a partir de um Llama local |
| **4 — Formatos I** | Texto, ELF, DEFLATE, ZIP, **PDF** | Extração de PDFs de teste (texto, tabelas, multi-coluna) melhor que `pdftotext` no corpus de avaliação |
| **5 — Agente I** | Ferramentas de shell, arquivos, auditoria, reversão e executor de capacidades | Lia executa tarefa de várias etapas com plano, aprovação, log e desfazer |
| **6 — Inferência própria** | GGUF + kernels ASM | Saída igual ao llama.cpp; tokens/s ≥ llama.cpp na mesma CPU |
| **7 — GUI e Agente II** | GUI nativa, teclado/mouse/tela, hardware, serviços | Lia opera uma aplicação gráfica com confirmação e lê sensores autorizados |
| **8 — Formatos II** | Imagens, áudio, Office, SQLite | |
| **9 — Treino local** | LoRA | Adaptador melhora métrica em tarefa da própria máquina |
| **10 — Rede Lia** | Compartilhamento assinado | Duas instalações trocam um adaptador |
| **11 — Portes** | AArch64, Windows, macOS | |

---

## 6. Convenções

- Idioma do código e da documentação: **português** (identificadores sem acento: `lia_ler`, `formatos/pdf`). **[aberta]** manter nomes de syscalls em inglês (`lia_read`) ou traduzir.
- Prefixo `lia_` em todo símbolo exportado.
- Cada arquivo `.S` começa com cabeçalho: propósito, entradas (registradores), saídas, registradores destruídos.
- Commits pequenos, mensagem em português no imperativo.
- Nenhuma dependência externa entra no núcleo sem decisão registrada neste plano.

## 7. Medição

Performance é requisito, então é medida desde a fase 1:
- `testes/bench/` com vazão (MiB/s), latência, ciclos (`rdtsc`/`perf`), memória máxima.
- Cada PR que toca o núcleo reporta antes/depois.

## 8. Questões em aberto

1. Assembler definitivo para x86-64 (GAS vs NASM/FASM).
2. Quantização padrão do modelo inicial.
3. Backend de GPU (Vulkan vs CUDA/ROCm).
4. Protocolo da Rede Lia.
5. Licença do projeto.
6. Tradução ou não dos nomes de primitivas POSIX.

## 9. Modus operandi

1. **Validação real antes de todo commit**, na máquina Arch Linux de desenvolvimento: compilar com `-Wall -Wextra -Werror`, rodar `make teste` e conferir contra programas de referência (`cat`, `sudo`, enumeração independente em shell). Nenhum arquivo é commitado sem ter sido executado.
2. **Conhecer a máquina antes de agir**: `lia tools discover` inventaria os executáveis (PATH na ordem, diretórios convencionais, `~/.local/bin`), `lia tools probe --safe` testa os que têm probe conhecido e não destrutivo. Os demais ficam `NO_SAFE_PROBE`; a Lia nunca executa um comando desconhecido só para ver o que acontece.
3. **Mais baixo nível possível**: binários estáticos, sem libc, `_start` e syscalls em Assembly (`nucleo/x86_64/`), runtime em C freestanding (`runtime/`).
4. A especificação da CLI está em [docs/LIA-CLI.md](docs/LIA-CLI.md) e deve evoluir junto deste plano; CLI, GUI e protocolo local compartilham os mesmos contratos.

## 10. Próximo passo imediato

1. ~~Registrar threat model e matriz de capacidades.~~ Feito: [docs/THREAT-MODEL.md](docs/THREAT-MODEL.md).
2. ~~Definir o protocolo de eventos compartilhado por CLI e GUI.~~ Feito: [docs/protocolo.md](docs/protocolo.md).
3. ~~Implementar `lia tools` em modo somente leitura, com manifestos e auditoria.~~ Feito: `cli/ferramentas.c`, `cli/sistema.c`, [docs/manifesto.md](docs/manifesto.md).
4. ~~Implementar `lia-echo`, `lia-cat`, testes de ABI e benchmark reproduzível.~~ Feito: `nucleo/x86_64/lia-echo.S`, `nucleo/x86_64/lia-cat.S`, implementações de referência em C (`nucleo/referencia/`), [docs/abi.md](docs/abi.md), `testes/bench/bench_cat.sh` (`make bench`).
5. `lia-hex`, `lia-tipo` (restante da fase 1) e `lia_memcpy`/`lia_memset`/`lia_memcmp` com seleção de implementação por `cpuid` (SSE2 → AVX2 → AVX-512), cada um com benchmark antes/depois.
6. Escolher toolkit GUI por uma prova de conceito que rode em Linux x86-64 e AArch64.
7. Só depois habilitar ferramentas de alteração, cada uma com simulação, confirmação e reversão.
