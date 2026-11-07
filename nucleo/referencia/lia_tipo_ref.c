/*
 * lia-tipo-ref — implementação de referência em C de nucleo/x86_64/lia-tipo.S.
 *
 * Mesmas assinaturas, mesma ordem de verificação, mesmo limite de 32 bytes
 * de prefixo. Usa só runtime/lia.h — nenhuma libc.
 */
#include "../../runtime/lia.h"

#define TAM_BUF 32

typedef struct {
    const unsigned char *bytes;
    size_t len;
    size_t offset;
    const char *nome;
} Assinatura;

static const unsigned char SIG_ELF[] = {0x7f, 'E', 'L', 'F'};
static const unsigned char SIG_PNG[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
static const unsigned char SIG_JPEG[] = {0xff, 0xd8, 0xff};
static const unsigned char SIG_ZIP1[] = {'P', 'K', 0x03, 0x04};
static const unsigned char SIG_ZIP2[] = {'P', 'K', 0x05, 0x06};
static const unsigned char SIG_ZIP3[] = {'P', 'K', 0x07, 0x08};
static const unsigned char SIG_GZIP[] = {0x1f, 0x8b};
static const unsigned char SIG_PDF[] = {'%', 'P', 'D', 'F', '-'};
static const unsigned char SIG_RIFF[] = {'R', 'I', 'F', 'F'};
static const unsigned char SIG_WAVE[] = {'W', 'A', 'V', 'E'};

/* casa(buf, n, offset, sig, sig_len): só "casa" se os sig_len bytes
 * couberem por completo dentro de n a partir de offset — nunca olha além
 * do que foi de fato lido (falha segura para prefixo truncado). */
static int casa(const unsigned char *buf, size_t n, size_t offset, const unsigned char *sig, size_t sig_len)
{
    if (offset + sig_len > n)
        return 0;
    for (size_t i = 0; i < sig_len; i++)
        if (buf[offset + i] != sig[i])
            return 0;
    return 1;
}

static const char *detectar_tipo(const unsigned char *buf, size_t n)
{
    if (casa(buf, n, 0, SIG_ELF, sizeof SIG_ELF))
        return "ELF";
    if (casa(buf, n, 0, SIG_PNG, sizeof SIG_PNG))
        return "PNG";
    if (casa(buf, n, 0, SIG_JPEG, sizeof SIG_JPEG))
        return "JPEG";
    if (casa(buf, n, 0, SIG_ZIP1, sizeof SIG_ZIP1) || casa(buf, n, 0, SIG_ZIP2, sizeof SIG_ZIP2) ||
        casa(buf, n, 0, SIG_ZIP3, sizeof SIG_ZIP3))
        return "ZIP";
    if (casa(buf, n, 0, SIG_GZIP, sizeof SIG_GZIP))
        return "GZIP";
    if (casa(buf, n, 0, SIG_PDF, sizeof SIG_PDF))
        return "PDF";
    if (casa(buf, n, 0, SIG_RIFF, sizeof SIG_RIFF) && casa(buf, n, 8, SIG_WAVE, sizeof SIG_WAVE))
        return "WAV";
    if (n == 0)
        return "VAZIO";
    for (size_t i = 0; i < n; i++) {
        unsigned char b = buf[i];
        if (b == 9 || b == 10 || b == 13)
            continue;
        if (b < 0x20 || b > 0x7e)
            return "DESCONHECIDO";
    }
    return "TEXTO";
}

int lia_main(long argc, char **argv, char **envp)
{
    (void)envp;
    int json = 0;
    const char *nome_arq = 0;

    for (long i = 1; i < argc; i++) {
        if (!lia_strcmp(argv[i], "--json")) {
            json = 1;
        } else if (!nome_arq) {
            nome_arq = argv[i];
        } else {
            lia_escrever_tudo(2, "uso: lia-tipo-ref [--json] [arquivo]\n",
                               lia_strlen("uso: lia-tipo-ref [--json] [arquivo]\n"));
            lia_exit(2);
        }
    }

    long fd = 0;
    int ehstdin = !nome_arq || (nome_arq[0] == '-' && nome_arq[1] == 0);
    if (!ehstdin) {
        fd = lia_openat(AT_FDCWD, nome_arq, O_RDONLY, 0);
        if (fd < 0) {
            lia_escrever_tudo(2, "lia-tipo-ref: nao foi possivel abrir: ",
                               lia_strlen("lia-tipo-ref: nao foi possivel abrir: "));
            lia_escrever_tudo(2, nome_arq, lia_strlen(nome_arq));
            lia_escrever_tudo(2, "\n", 1);
            lia_exit(1);
        }
    }

    unsigned char buf[TAM_BUF];
    size_t n = 0;
    while (n < TAM_BUF) {
        long r = lia_read((int)fd, buf + n, TAM_BUF - n);
        if (r == -EINTR)
            continue;
        if (r < 0) {
            lia_escrever_tudo(2, "lia-tipo-ref: erro de leitura\n", lia_strlen("lia-tipo-ref: erro de leitura\n"));
            lia_exit(1);
        }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    if (!ehstdin)
        lia_close((int)fd);

    const char *tipo = detectar_tipo(buf, n);
    if (json) {
        lia_escrever_tudo(1, "{\"tipo\":\"", 9);
        lia_escrever_tudo(1, tipo, lia_strlen(tipo));
        lia_escrever_tudo(1, "\"}\n", 3);
    } else {
        lia_escrever_tudo(1, tipo, lia_strlen(tipo));
        lia_escrever_tudo(1, "\n", 1);
    }
    lia_exit(0);
}
