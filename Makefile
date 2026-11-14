CC      := gcc
CFLAGS  := -std=c17 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
           -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables \
           -fno-tree-loop-distribute-patterns
LDFLAGS := -nostdlib -static -no-pie

B := build

RUNTIME := $(B)/entrada.o $(B)/sys.o $(B)/texto.o
CLI     := $(B)/main.o $(B)/ferramentas.o $(B)/privilegio.o $(B)/sistema.o

REF := $(B)/entrada.o $(B)/sys.o $(B)/texto.o

# lia-mem: primitivas de memória (escalar/SSE2/AVX2) + despacho por CPUID.
MEM := $(B)/entrada.o $(B)/sys.o $(B)/texto.o $(B)/lia-cpuid.o $(B)/lia-mem.o \
       $(B)/lia_mem.o $(B)/lia_mem_ref.o

all: $(B)/lia $(B)/lia-cat $(B)/lia-echo $(B)/lia-hex $(B)/lia-tipo \
     $(B)/lia-cat-ref $(B)/lia-echo-ref $(B)/lia-hex-ref $(B)/lia-tipo-ref \
     $(B)/lia-mem-teste $(B)/lia-mem-bench $(B)/lia-processos-teste

$(B):
	mkdir -p $(B)

$(B)/entrada.o: nucleo/x86_64/entrada.S | $(B)
	$(CC) -c $< -o $@

$(B)/lia-cat.o: nucleo/x86_64/lia-cat.S | $(B)
	$(CC) -c $< -o $@

$(B)/lia-echo.o: nucleo/x86_64/lia-echo.S | $(B)
	$(CC) -c $< -o $@

$(B)/lia-hex.o: nucleo/x86_64/lia-hex.S | $(B)
	$(CC) -c $< -o $@

$(B)/lia-tipo.o: nucleo/x86_64/lia-tipo.S | $(B)
	$(CC) -c $< -o $@

$(B)/lia-cpuid.o: nucleo/x86_64/lia-cpuid.S | $(B)
	$(CC) -c $< -o $@

$(B)/lia-mem.o: nucleo/x86_64/lia-mem.S | $(B)
	$(CC) -c $< -o $@

$(B)/%.o: runtime/%.c runtime/lia.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/%.o: cli/%.c runtime/lia.h cli/cli.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/%_ref.o: nucleo/referencia/%_ref.c runtime/lia.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/teste_mem.o: testes/mem/teste_mem.c runtime/lia.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/bench_mem.o: testes/mem/bench_mem.c runtime/lia.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/teste_processos_top_cpu.o: testes/nucleo/teste_processos_top_cpu.c runtime/lia.h cli/cli.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/lia: $(RUNTIME) $(CLI)
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-cat: $(B)/lia-cat.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-echo: $(B)/lia-echo.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-hex: $(B)/lia-hex.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-tipo: $(B)/lia-tipo.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-cat-ref: $(REF) $(B)/lia_cat_ref.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-echo-ref: $(REF) $(B)/lia_echo_ref.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-hex-ref: $(REF) $(B)/lia_hex_ref.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-tipo-ref: $(REF) $(B)/lia_tipo_ref.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-mem-teste: $(MEM) $(B)/teste_mem.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-mem-bench: $(MEM) $(B)/bench_mem.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-processos-teste: $(RUNTIME) $(B)/sistema.o $(B)/teste_processos_top_cpu.o
	$(CC) $(LDFLAGS) $^ -o $@

teste: all
	sh testes/teste.sh

bench: all
	sh testes/bench/bench_cat.sh
	sh testes/mem/bench_mem.sh

clean:
	rm -rf $(B)

# ---------------------------------------------------------------------------
# GUI (gui/): camada fina sobre docs/protocolo.md, fora do núcleo sem libc.
# Toolchain própria (GTK4 + GLib + json-glib, via pkg-config) — não entra em
# `all`/`teste`/`bench` de propósito: o build do núcleo (-ffreestanding
# -nostdlib) nunca depende de bibliotecas dinâmicas do sistema, e a GUI é
# opcional nesta etapa. Ver docs/GUI-POC.md.
GUI_PKGS        := gtk4 gio-unix-2.0 json-glib-1.0
GUI_CFLAGS      := -std=c17 -O2 -Wall -Wextra -Werror $(shell pkg-config --cflags $(GUI_PKGS))
GUI_LIBS        := $(shell pkg-config --libs $(GUI_PKGS))
TESTE_GUI_PKGS  := json-glib-1.0
TESTE_GUI_CFLAGS := -std=c17 -O2 -Wall -Wextra -Werror $(shell pkg-config --cflags $(TESTE_GUI_PKGS))
TESTE_GUI_LIBS  := $(shell pkg-config --libs $(TESTE_GUI_PKGS))

$(B)/gui-protocolo.o: gui/protocolo.c gui/protocolo.h | $(B)
	$(CC) $(GUI_CFLAGS) -c $< -o $@

# Compilação própria para make teste-gui: só GLib/json-glib (sem -I/-l de
# GTK), provando que gui/protocolo.c é mesmo testável sem toolkit gráfico.
$(B)/gui-protocolo-teste.o: gui/protocolo.c gui/protocolo.h | $(B)
	$(CC) $(TESTE_GUI_CFLAGS) -c $< -o $@

$(B)/gui-teste-protocolo.o: gui/teste_protocolo.c gui/protocolo.h | $(B)
	$(CC) $(TESTE_GUI_CFLAGS) -c $< -o $@

$(B)/gui-lia-gui.o: gui/lia-gui.c gui/protocolo.h | $(B)
	$(CC) $(GUI_CFLAGS) -c $< -o $@

$(B)/lia-gui: $(B)/gui-lia-gui.o $(B)/gui-protocolo.o
	$(CC) $^ $(GUI_LIBS) -o $@

$(B)/lia-gui-teste: $(B)/gui-teste-protocolo.o $(B)/gui-protocolo-teste.o
	$(CC) $^ $(TESTE_GUI_LIBS) -o $@

gui: $(B)/lia-gui

teste-gui: $(B)/lia-gui-teste
	$(B)/lia-gui-teste

gui-run: gui
	$(B)/lia-gui

gui-smoke: gui
	sh gui/teste_fumaca.sh

# ---------------------------------------------------------------------------
# sessiond (sessiond/): daemon de sessão — servidor do protocolo NDJSON e
# máquina de estados. Executor plugável (sessiond/executor.h): real por
# padrão (sessiond/executor_real.c, chama cli/sistema.c e cli/ferramentas.c
# de verdade, limitado às seis ferramentas de leitura de
# sessiond/manifesto_real.c — docs/manifesto.md); LIA_SESSIOND_EXECUTOR=mock
# troca para o simulado (sessiond/executor_mock.c), só para testes unitários.
#
# cli/sistema.c e cli/ferramentas.c são código do núcleo sem libc — para
# rodar dentro deste processo hospedado (GLib), são recompilados em -fPIC
# (prefixo sessiond-pic-*, nunca reaproveita os .o não-PIC de build/ do
# núcleo) e ligados com sessiond/lia_syscall_hospedado.c no lugar de
# nucleo/x86_64/entrada.S (que define _start — colidiria com o do daemon).
SESSIOND_PKGS   := gio-unix-2.0 json-glib-1.0
SESSIOND_CFLAGS := -std=c17 -O2 -Wall -Wextra -Werror $(shell pkg-config --cflags $(SESSIOND_PKGS))
SESSIOND_LIBS   := $(shell pkg-config --libs $(SESSIOND_PKGS))
SESSIOND_PIC_CFLAGS := -std=c17 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fPIC

SESSIOND_OBJS := $(B)/sessiond-sessiond.o $(B)/sessiond-executor-comum.o \
                  $(B)/sessiond-executor-mock.o $(B)/sessiond-manifesto-mock.o \
                  $(B)/sessiond-executor-real.o $(B)/sessiond-manifesto-real.o \
                  $(B)/sessiond-protocolo.o $(B)/sessiond-syscall.o \
                  $(B)/sessiond-pic-texto.o $(B)/sessiond-pic-sys.o \
                  $(B)/sessiond-pic-sistema.o $(B)/sessiond-pic-ferramentas.o \
                  $(B)/sessiond-modelo-comum.o $(B)/sessiond-modelo-mock.o \
                  $(B)/sessiond-modelo-ollama.o $(B)/sessiond-cliente-ollama.o \
                  $(B)/sessiond-modelo-llama-cpp.o $(B)/sessiond-cliente-llama-cpp.o \
                  $(B)/sessiond-prompt.o

$(B)/sessiond-protocolo.o: gui/protocolo.c gui/protocolo.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-syscall.o: sessiond/lia_syscall_hospedado.c runtime/lia.h cli/cli.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-manifesto-mock.o: sessiond/manifesto_mock.c sessiond/manifesto_mock.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-executor-mock.o: sessiond/executor_mock.c sessiond/executor.h sessiond/manifesto_mock.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-manifesto-real.o: sessiond/manifesto_real.c sessiond/manifesto_real.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-executor-real.o: sessiond/executor_real.c sessiond/executor.h sessiond/manifesto_real.h runtime/lia.h cli/cli.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-executor-comum.o: sessiond/executor_comum.c sessiond/executor.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-sessiond.o: sessiond/sessiond.c sessiond/executor.h sessiond/modelo.h sessiond/prompt.h gui/protocolo.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-modelo-comum.o: sessiond/modelo_comum.c sessiond/modelo.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-modelo-mock.o: sessiond/modelo_mock.c sessiond/modelo.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-modelo-ollama.o: sessiond/modelo_ollama.c sessiond/modelo.h ollama/cliente_ollama.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-cliente-ollama.o: ollama/cliente_ollama.c ollama/cliente_ollama.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-modelo-llama-cpp.o: sessiond/modelo_llama_cpp.c sessiond/modelo.h ollama/cliente_llama_cpp.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-cliente-llama-cpp.o: ollama/cliente_llama_cpp.c ollama/cliente_llama_cpp.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-prompt.o: sessiond/prompt.c sessiond/prompt.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/sessiond-pic-texto.o: runtime/texto.c runtime/lia.h | $(B)
	$(CC) $(SESSIOND_PIC_CFLAGS) -c $< -o $@

$(B)/sessiond-pic-sys.o: runtime/sys.c runtime/lia.h | $(B)
	$(CC) $(SESSIOND_PIC_CFLAGS) -c $< -o $@

$(B)/sessiond-pic-sistema.o: cli/sistema.c runtime/lia.h cli/cli.h | $(B)
	$(CC) $(SESSIOND_PIC_CFLAGS) -c $< -o $@

$(B)/sessiond-pic-ferramentas.o: cli/ferramentas.c runtime/lia.h cli/cli.h | $(B)
	$(CC) $(SESSIOND_PIC_CFLAGS) -c $< -o $@

$(B)/lia-sessiond: $(SESSIOND_OBJS)
	$(CC) $^ $(SESSIOND_LIBS) -o $@

sessiond: $(B)/lia-sessiond

teste-sessiond: sessiond
	sh testes/integration/teste_sessiond_basico.sh

integration: sessiond gui
	sh testes/integration/teste_integracao.sh

teste-modelo: sessiond
	sh testes/integration/teste_modelo.sh

$(B)/teste-cliente-ollama.o: testes/integration/teste_cliente_ollama.c sessiond/modelo.h ollama/cliente_ollama.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/teste-cliente-ollama: $(B)/teste-cliente-ollama.o $(B)/sessiond-cliente-ollama.o $(B)/sessiond-modelo-ollama.o $(B)/sessiond-modelo-comum.o
	$(CC) $^ $(SESSIOND_LIBS) -o $@

teste-ollama: $(B)/teste-cliente-ollama
	sh testes/integration/teste_cliente_ollama.sh

$(B)/teste-cliente-llama-cpp.o: testes/integration/teste_cliente_llama_cpp.c sessiond/modelo.h ollama/cliente_llama_cpp.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) -c $< -o $@

$(B)/teste-cliente-llama-cpp: $(B)/teste-cliente-llama-cpp.o $(B)/sessiond-cliente-llama-cpp.o $(B)/sessiond-modelo-llama-cpp.o $(B)/sessiond-modelo-comum.o
	$(CC) $^ $(SESSIOND_LIBS) -o $@

teste-llama-cpp: $(B)/teste-cliente-llama-cpp
	sh testes/integration/teste_cliente_llama_cpp.sh

# ---------------------------------------------------------------------------
# lia-chat (chat/): primeiro cliente de chat interativo de terminal para o
# protocolo (docs/protocolo.md §9). Camada fina sobre gui/protocolo.c — não
# duplica a máquina de estados do protocolo nem a lógica do daemon; reaproveita
# diretamente os objetos de sessiond/modelo_*.c e ollama/cliente_ollama.c já
# compilados para "make sessiond" (mesmo probe de disponibilidade que o
# daemon usaria, nunca a aprovação/execução em si). PODE enviar 'aprovar'
# (desliga modo_leitura explicitamente) — é o próprio ponto desta CLI,
# diferente de lia-gui. Edição de linha real (setas, histórico, Ctrl-A/E/
# K/W, copiar/colar) via libedit — BSD, não GNU readline (GPL): a licença
# do projeto Lia ainda está em aberto (PLANO.md §8), e libedit evita
# prender o binário inteiro a GPL caso ele venha a ser distribuído.
CHAT_PKGS   := libedit
CHAT_CFLAGS := $(shell pkg-config --cflags $(CHAT_PKGS))
CHAT_LIBS   := $(shell pkg-config --libs $(CHAT_PKGS))

$(B)/chat-lia-chat.o: chat/lia-chat.c gui/protocolo.h sessiond/modelo.h sessiond/prompt.h | $(B)
	$(CC) $(SESSIOND_CFLAGS) $(CHAT_CFLAGS) -c $< -o $@

$(B)/lia-chat: $(B)/chat-lia-chat.o $(B)/sessiond-protocolo.o $(B)/sessiond-modelo-mock.o \
               $(B)/sessiond-modelo-ollama.o $(B)/sessiond-modelo-comum.o $(B)/sessiond-cliente-ollama.o \
               $(B)/sessiond-modelo-llama-cpp.o $(B)/sessiond-cliente-llama-cpp.o
	$(CC) $^ $(SESSIOND_LIBS) $(CHAT_LIBS) -o $@

chat: $(B)/lia-chat

teste-chat: chat sessiond
	sh testes/integration/teste_chat.sh

chat-run: chat
	$(B)/lia-chat

.PHONY: all teste bench clean gui teste-gui gui-run gui-smoke sessiond teste-sessiond integration teste-modelo teste-ollama teste-llama-cpp chat teste-chat chat-run
