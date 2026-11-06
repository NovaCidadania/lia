# PLANO — Lia

Documento vivo. Define a visão, as decisões técnicas, as fases e os critérios de conclusão do projeto. Decisões marcadas **[aberta]** ainda precisam ser tomadas.

---

## 1. Visão

Uma IA **local**, **altamente performática** e **agêntica**, construída de baixo para cima sobre as duas funções básicas de qualquer IA: **ler** e **interpretar** dados.

- Lê **qualquer** arquivo do sistema a partir dos bytes, com um decodificador próprio por formato.
- Opera o computador por completo: arquivos, processos, configuração, entrada/saída, privilégios administrativos e hardware.
- Mantém contexto próprio em `~/.lia`, separando dados para humanos de dados para máquina.
- Treina localmente e compartilha conhecimento com outras instalações.

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

### L4 — Agente

A Lia age no computador por meio de **ferramentas** com interface uniforme (nome, parâmetros, efeito, reversibilidade).

| Domínio | Linux (1ª plataforma) | Windows / macOS (depois) |
|---|---|---|
| Comandos do SO | `fork`/`execve`, pipes, captura de saída | `CreateProcess` / `posix_spawn` |
| Arquivos e configuração | núcleo L0 + decodificadores L2 | idem |
| Teclado e mouse (saída) | `/dev/uinput` | `SendInput` / CGEvent |
| Teclado e mouse (entrada) | `/dev/input/event*` (evdev) | Raw Input / IOHID |
| Tela | DRM/KMS, X11 (`XGetImage`), Wayland (portal/`wlr-screencopy`) | DXGI / ScreenCaptureKit |
| Privilégios | `sudo`, `pkexec`, capabilities (`CAP_*`), polkit | UAC / Admin, `authorizationdb` |
| Serviços | systemd (D-Bus) | SCM / launchd |
| Hardware | `/sys`, `/proc`, PCI config space, USB (`usbfs`), I²C/SMBus, sensores (hwmon), `ioctl`, MSR (`/dev/cpu/*/msr`) | WMI, IOKit |

**Privilégios — poder total com rastreabilidade:**

A Lia é projetada para operar como root/Admin. Para isso ser sustentável no PC de uma pessoa:

1. **Níveis de poder** configuráveis em `~/.lia/humano/permissoes.toml`: `leitura` → `usuario` → `admin` → `hardware`. O usuário define até onde a Lia vai sem perguntar.
2. **Processo auxiliar privilegiado separado** (`lia-raiz`): o processo principal roda como usuário; ações root passam por um socket Unix para um auxiliar pequeno e auditável. Isso mantém o código que roda como root mínimo.
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

---

## 5. Fases

| Fase | Entrega | Critério de conclusão |
|---|---|---|
| **0 — Fundação** | Repositório, `make`, ABI documentada, CI (build + testes) | `make && make teste` passa limpo |
| **1 — Núcleo de I/O** | Primitivas L0 + `lia-echo`, `lia-cat`, `lia-hex`, `lia-tipo` em Assembly | Saída byte-a-byte idêntica a `cat`/`xxd`; vazão de `lia-cat` ≥ `cat` do coreutils em arquivos de 1 GiB |
| **2 — Runtime** | Alocador, strings, estruturas, config TOML | Testes contra oráculos em C; zero vazamento sob valgrind |
| **3 — Ponte com o modelo** | Cliente Ollama em C, `lia pergunta "..."` | Resposta em streaming a partir de um Llama local |
| **4 — Formatos I** | Texto, ELF, DEFLATE, ZIP, **PDF** | Extração de PDFs de teste (texto, tabelas, multi-coluna) melhor que `pdftotext` no corpus de avaliação |
| **5 — Agente I** | Ferramentas de shell, arquivos, auditoria, reversão, `lia-raiz` | Lia executa uma tarefa de várias etapas (ex.: "configure X") com log e desfazer |
| **6 — Inferência própria** | GGUF + kernels ASM | Saída igual ao llama.cpp; tokens/s ≥ llama.cpp na mesma CPU |
| **7 — Agente II** | Teclado/mouse/tela, hardware, serviços | Lia opera uma aplicação gráfica e lê sensores |
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

## 9. Próximo passo imediato

Fase 0 + início da fase 1:
1. `Makefile` e estrutura de diretórios.
2. `nucleo/x86_64/syscall.S` com `lia_read`, `lia_write`, `lia_open`, `lia_close`, `lia_exit`.
3. `ferramentas/lia-cat.S` — primeiro binário da Lia, sem libc.
4. Teste comparando com `cat`.
