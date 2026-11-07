/*
 * lia-cat-ref — implementação de referência em C de nucleo/x86_64/lia-cat.S.
 *
 * Mesma entrada, mesma saída, mesmo código de saída: é o "oráculo" contra o
 * qual a versão em Assembly é comparada nos testes (PLANO.md §2.1, regra 1).
 * Usa só o runtime da Lia (runtime/lia.h) — nenhuma libc, mesmo sendo C.
 */
#include "../../runtime/lia.h"

#define TAM_BUF 65536
static char buf[TAM_BUF];

static int escrever_erro(const char *s, size_t n)
{
    return lia_escrever_tudo(2, s, n);
}

/* Copia fd -> stdout até EOF. Retorna 0 em sucesso, -1 em erro de I/O. */
static int copiar(int fd)
{
    for (;;) {
        long r = lia_read(fd, buf, TAM_BUF);
        if (r == -EINTR)
            continue;
        if (r < 0)
            return -1;
        if (r == 0)
            return 0; /* EOF */
        if (lia_escrever_tudo(1, buf, (size_t)r) < 0)
            return -1;
    }
}

int lia_main(long argc, char **argv, char **envp)
{
    (void)envp;
    int status = 0;

    if (argc < 2) {
        if (copiar(0) < 0)
            status = 1;
        lia_exit(status);
    }

    for (long i = 1; i < argc; i++) {
        const char *nome = argv[i];
        int ehstdin = nome[0] == '-' && nome[1] == 0;
        long fd = ehstdin ? 0 : lia_openat(AT_FDCWD, nome, O_RDONLY, 0);
        if (fd < 0) {
            status = 1;
            escrever_erro("lia-cat: nao foi possivel abrir: ", lia_strlen("lia-cat: nao foi possivel abrir: "));
            escrever_erro(nome, lia_strlen(nome));
            escrever_erro("\n", 1);
            continue;
        }
        if (copiar((int)fd) < 0) {
            status = 1;
            escrever_erro("lia-cat: erro de leitura ou escrita\n", lia_strlen("lia-cat: erro de leitura ou escrita\n"));
        }
        if (!ehstdin)
            lia_close((int)fd);
    }
    lia_exit(status);
}
