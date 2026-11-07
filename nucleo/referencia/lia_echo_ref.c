/*
 * lia-echo-ref — implementação de referência em C de nucleo/x86_64/lia-echo.S.
 *
 * Mesmo buffer de 4 KiB da versão em Assembly, de propósito: qualquer entrada
 * maior que isso exercita o laço de leitura/escrita parcial nas duas versões
 * da mesma forma, para a comparação byte a byte ter sentido.
 */
#include "../../runtime/lia.h"

#define TAM_BUF 4096
static char buf[TAM_BUF];

int lia_main(long argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    for (;;) {
        long r = lia_read(0, buf, TAM_BUF);
        if (r == -EINTR)
            continue;
        if (r < 0) {
            lia_escrever_tudo(2, "lia-echo: erro de leitura ou escrita\n", lia_strlen("lia-echo: erro de leitura ou escrita\n"));
            lia_exit(1);
        }
        if (r == 0)
            lia_exit(0); /* EOF */
        if (lia_escrever_tudo(1, buf, (size_t)r) < 0) {
            lia_escrever_tudo(2, "lia-echo: erro de leitura ou escrita\n", lia_strlen("lia-echo: erro de leitura ou escrita\n"));
            lia_exit(1);
        }
    }
}
