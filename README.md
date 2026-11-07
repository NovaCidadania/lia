# Lia

**Lia** é uma IA feita para rodar **localmente** em computadores pessoais: inferência, treinamento e memória na própria máquina, com postura **agêntica** completa — ela assume tarefas no PC, opera o sistema operacional e conversa com o hardware.

> Status: **início da implementação.** Veja o [PLANO.md](PLANO.md).

## Como usar hoje (Linux x86-64)

```sh
make            # compila build/lia, build/lia-cat, build/lia-echo e as
                 # implementações de referência em C (estáticos, sem libc)
make teste      # testes reais nesta máquina (214 testes)
make bench      # benchmarks reproduzíveis: lia-cat (ASM) vs C vs cat (coreutils);
                 # lia_memcpy/memset/memcmp por backend (escalar/SSE2/AVX2)

build/lia-echo                   # stdin -> stdout, sem libc (prova mínima do núcleo)
build/lia-cat arquivo1 arquivo2  # como cat, em Assembly puro
build/lia-hex arquivo            # despejo hexadecimal + ASCII, idêntico ao xxd
build/lia-tipo arquivo           # tipo pelos bytes mágicos (ELF/PDF/PNG/JPEG/ZIP/gzip/WAV/texto), nunca pela extensão
build/lia system diagnosticar    # distro, kernel, CPU, memória (somente leitura)
build/lia tools discover        # inventário dos executáveis do sistema (somente leitura)
build/lia tools probe --safe    # testa os que têm probe seguro conhecido, sem privilégio
build/lia tools list --estado PROBE_SUCCESS
build/lia shell                 # abre o shell do usuário, explicitamente solicitado
build/lia root --status         # diz se o `sudo` do sistema concederia root agora, sem elevar nada
build/lia root --auth           # pede ao `sudo` real para autenticar nesta sessão (a senha fica só com o sudo)
build/lia root -- id -u         # executa um comando concreto como root, com auditoria e autenticação a cada vez
build/lia-mem-teste              # lia_memcpy/memset/memcmp: escalar/SSE2/AVX2 vs referência em C
build/lia-mem-bench              # benchmark por backend e tamanho (ciclos/byte, MiB/s)
```

## GUI (prova de conceito, somente leitura)

A GUI (`gui/`) é uma camada fina sobre o protocolo NDJSON ([docs/protocolo.md](docs/protocolo.md)):
não há núcleo nem lógica de execução aqui, só apresentação. Diferente do resto
do repositório, usa libc e bibliotecas dinâmicas normais (GTK4/GLib/json-glib)
e por isso **não** entra em `make`/`make teste`/`make bench` — é um build
separado, opcional, que precisa de `pkg-config` + `gtk4` + `json-glib-1.0`
instalados (ver [docs/GUI-POC.md](docs/GUI-POC.md) para a decisão de toolkit).

```sh
make gui          # compila build/lia-gui (requer GTK4/GLib/json-glib via pkg-config)
make teste-gui     # testes de gui/protocolo.c — parsing/validação NDJSON, sem display (33 testes)
make gui-run       # compila e abre a janela
make gui-smoke     # com display: confere ausência de sudo/pkexec/exec* e execução sem daemon
```

Nesta primeira etapa a GUI é somente leitura: o campo de entrada não envia
nada ao daemon (só ecoa localmente como rascunho), e `gui/protocolo.c` recusa,
antes de qualquer tentativa de envio, qualquer mensagem com efeito
(`solicitar`/`aprovar`) — só `cancelar`/`assinar` são permitidos.

## Daemon de sessão (`lia-sessiond`, protocolo completo)

`sessiond/` implementa o outro lado do protocolo: um servidor de socket Unix
e máquina de estados, com um **executor simulado** (nenhuma ferramenta real
é executada, nenhum privilégio é pedido — ver [docs/protocolo.md](docs/protocolo.md) §8).
Mesma toolchain hospedada que a GUI (GLib/GIO/json-glib), fora de
`make`/`make teste`/`make bench`.

```sh
make sessiond        # compila build/lia-sessiond
make teste-sessiond   # inicia o daemon num $HOME temporário, testa o básico, encerra
make integration       # suíte completa: plano/aprovação/cancelamento/erros/
                       # múltiplos clientes/socket stale/GUI com e sem daemon
```

`lia-sessiond` escuta em `~/.lia/run/session.sock` (modo `0600`, diretório
`0700`), nunca roda como root, termina de forma limpa com `SIGTERM`/`SIGINT`
(remove o próprio socket ao sair), e só aceita conexões do mesmo uid
(`SO_PEERCRED` — limitações documentadas em `docs/protocolo.md` §7).
Encerrar: `kill <pid>` ou `Ctrl-C` em primeiro plano.

A Lia nunca guarda senha, nunca altera `/etc/sudoers` por conta própria e nunca mantém root como estado permanente — toda elevação passa pelo `sudo` real do sistema, caso a caso. Veja a seção de privilégios do [PLANO.md](PLANO.md).

## Princípios

1. **Local primeiro.** Nada sai da máquina sem decisão explícita do usuário.
2. **Performance acima de conveniência.** Núcleo em **Assembly** (nos caminhos críticos medidos por benchmark), suporte em **C**, sem dependências pesadas.
3. **Ler e interpretar dados — de verdade.** Lia parte dos bytes, não do texto. Todo arquivo do sistema (PDF, imagem, binário ELF, áudio, planilha, banco de dados) é lido por um decodificador próprio, escrito para aquele formato.
4. **A máquina é o contexto.** Lia prefere caminhos voltados ao sistema em que está instalada (syscalls, `/proc`, `/sys`, dispositivos) a interpretar texto puro.
5. **Capacidade, não acesso irrestrito, e privilégio mínimo e temporário.** Cada ferramenta declara o que precisa (arquivos, entrada/saída, rede, hardware, administração) e o risco da ação. Elevar a root/Admin usa sempre a API oficial do sistema (`sudo`, polkit, UAC), uma ação concreta por vez, com autenticação da pessoa a cada vez — nunca senha guardada, nunca alteração automática de `sudoers`, nunca bypass de autenticação. Toda ação com efeito é registrada antes e depois, de forma auditável e, quando possível, reversível.
6. **Aprende e compartilha.** Treinamento local (adaptadores) e troca de conhecimento entre instalações, só após aprovação e com pacotes verificáveis.

## Arquitetura (resumo)

```
 ┌────────────────────────────────────────────────────────────┐
 │ L6  Rede Lia      compartilhamento entre instalações       │
 │ L5  Treino        ajuste fino local (LoRA/adaptadores)     │
 │ L4  Agente        shell, arquivos, input, privilégios, HW  │
 │ L3  Inferência    Ollama/Llama (início) → motor próprio    │
 │ L2  Formatos      decodificadores puros por tipo de arquivo│
 │ L1  Runtime C     estruturas, alocador, utilidades         │
 │ L0  Núcleo ASM    syscalls, I/O, memória, SIMD             │
 └────────────────────────────────────────────────────────────┘
          contexto próprio em ~/.lia  (humano/ | maquina/)
```

Detalhes de cada camada, fases e critérios de conclusão estão no [PLANO.md](PLANO.md).

## Modelo inicial

O desenvolvimento começa com um modelo **Llama open source já treinado**, servido via **Ollama** em `localhost`. Isso permite construir o núcleo, os decodificadores e o agente enquanto o motor de inferência próprio (GGUF + kernels em Assembly) é desenvolvido em paralelo.

## Plataformas

| Plataforma        | Prioridade |
|-------------------|------------|
| Linux x86-64      | 1ª         |
| Linux AArch64     | 2ª         |
| Windows x86-64    | 3ª         |
| macOS AArch64     | 4ª         |

## Contexto próprio: `~/.lia`

```
~/.lia/
├── humano/    # legível e editável por pessoas (Markdown, TOML, texto)
└── maquina/   # formatos binários próprios da Lia (índices, vetores, caches, pesos)
```

## Estrutura do repositório (planejada)

```
lia/
├── nucleo/        # L0 – Assembly (x86_64/, aarch64/)
├── runtime/       # L1 – C
├── formatos/      # L2 – um decodificador por formato
├── inferencia/    # L3
├── agente/        # L4
├── treino/        # L5
├── rede/          # L6
├── ferramentas/   # utilitários de linha de comando (lia-cat, lia-hex, ...)
├── testes/
└── docs/
```

## Licença

A definir.
