CC      := gcc
CFLAGS  := -std=c17 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
           -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables \
           -fno-tree-loop-distribute-patterns
LDFLAGS := -nostdlib -static -no-pie

B := build

RUNTIME := $(B)/entrada.o $(B)/sys.o $(B)/texto.o
CLI     := $(B)/main.o $(B)/ferramentas.o $(B)/privilegio.o $(B)/sistema.o

REF := $(B)/entrada.o $(B)/sys.o $(B)/texto.o

all: $(B)/lia $(B)/lia-cat $(B)/lia-echo $(B)/lia-cat-ref $(B)/lia-echo-ref

$(B):
	mkdir -p $(B)

$(B)/entrada.o: nucleo/x86_64/entrada.S | $(B)
	$(CC) -c $< -o $@

$(B)/lia-cat.o: nucleo/x86_64/lia-cat.S | $(B)
	$(CC) -c $< -o $@

$(B)/lia-echo.o: nucleo/x86_64/lia-echo.S | $(B)
	$(CC) -c $< -o $@

$(B)/%.o: runtime/%.c runtime/lia.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/%.o: cli/%.c runtime/lia.h cli/cli.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/%_ref.o: nucleo/referencia/%_ref.c runtime/lia.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/lia: $(RUNTIME) $(CLI)
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-cat: $(B)/lia-cat.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-echo: $(B)/lia-echo.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-cat-ref: $(REF) $(B)/lia_cat_ref.o
	$(CC) $(LDFLAGS) $^ -o $@

$(B)/lia-echo-ref: $(REF) $(B)/lia_echo_ref.o
	$(CC) $(LDFLAGS) $^ -o $@

teste: all
	sh testes/teste.sh

bench: all
	sh testes/bench/bench_cat.sh

clean:
	rm -rf $(B)

.PHONY: all teste bench clean
