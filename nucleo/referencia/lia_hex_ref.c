/*
 * lia-hex-ref — implementação de referência em C de nucleo/x86_64/lia-hex.S.
 *
 * Mesmo algoritmo, mesmo formato (idêntico a `xxd` sem opções: ver o
 * cabeçalho do .S para a derivação do formato), usado como oráculo pelos
 * testes. Usa só runtime/lia.h — nenhuma libc.
 */
#include "../../runtime/lia.h"

#define TAM_LEITURA 4096
#define TAM_SAIDA 8192

static char entrada[TAM_LEITURA];
static char linha[16];
static char saida[TAM_SAIDA];
static size_t saida_n;
static u64 offset;

static void flush_saida(void)
{
    if (saida_n) {
        lia_escrever_tudo(1, saida, saida_n);
        saida_n = 0;
    }
}

static void emit(const void *p, size_t n)
{
    memcpy(saida + saida_n, p, n);
    saida_n += n;
}

static void emit_c(char c) { saida[saida_n++] = c; }

static void emit_hex2(unsigned char b)
{
    static const char dig[] = "0123456789abcdef";
    emit_c(dig[b >> 4]);
    emit_c(dig[b & 0xf]);
}

/* Offset em hex, mínimo 8 dígitos, crescendo sozinho se não couber (igual
 * ao xxd): calcula os 16 nibbles e emite a partir do primeiro não-zero
 * entre os 8 mais significativos (ou a partir do nibble 8 se todos forem
 * zero, garantindo pelo menos 8 dígitos). */
static void emit_offset(u64 v)
{
    static const char dig[] = "0123456789abcdef";
    char nibbles[16];
    for (int i = 15; i >= 0; i--) {
        nibbles[i] = dig[v & 0xf];
        v >>= 4;
    }
    int inicio = 0;
    while (inicio < 8 && nibbles[inicio] == '0')
        inicio++;
    if (inicio == 8) {
        /* todos os 8 mais significativos são zero: ainda assim 8 dígitos */
    } else {
        /* achou um não-zero antes do 8º: começa dali (pode ser < 8 só se
         * o valor realmente precisar de mais de 16 dígitos, o que não
         * acontece aqui por v ser u64; então inicio fica em 0..7 e o
         * total emitido é 16-inicio >= 9, crescendo como o xxd). */
    }
    emit(nibbles + inicio, (size_t)(16 - inicio));
}

/* Monta e emite uma linha completa a partir de 'linha'/n/offset. */
static void formatar_linha(size_t n)
{
    if (saida_n > TAM_SAIDA - 128)
        flush_saida();

    emit_offset(offset);
    emit_c(':');

    size_t col = 0; /* colunas já emitidas no campo hex */
    for (int g = 0; g < 8; g++) {
        size_t i0 = (size_t)g * 2, i1 = i0 + 1;
        if (i0 >= n)
            break;
        emit_c(' ');
        col++;
        emit_hex2((unsigned char)linha[i0]);
        col += 2;
        if (i1 < n) {
            emit_hex2((unsigned char)linha[i1]);
            col += 2;
        }
    }
    for (size_t pad = 40 - col; pad > 0; pad--)
        emit_c(' ');
    emit_c(' ');
    emit_c(' ');

    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)linha[i];
        emit_c((b >= 0x20 && b <= 0x7e) ? (char)b : '.');
    }
    emit_c('\n');
}

int lia_main(long argc, char **argv, char **envp)
{
    (void)envp;
    int status = 0;
    long fd = 0;

    if (argc > 2) {
        lia_escrever_tudo(2, "uso: lia-hex-ref [arquivo]\n", lia_strlen("uso: lia-hex-ref [arquivo]\n"));
        lia_exit(2);
    }
    if (argc == 2) {
        const char *nome = argv[1];
        int ehstdin = nome[0] == '-' && nome[1] == 0;
        if (!ehstdin) {
            fd = lia_openat(AT_FDCWD, nome, O_RDONLY, 0);
            if (fd < 0) {
                lia_escrever_tudo(2, "lia-hex-ref: nao foi possivel abrir: ", lia_strlen("lia-hex-ref: nao foi possivel abrir: "));
                lia_escrever_tudo(2, nome, lia_strlen(nome));
                lia_escrever_tudo(2, "\n", 1);
                lia_exit(1);
            }
        }
    }

    size_t nlinha = 0;
    for (;;) {
        long r = lia_read((int)fd, entrada, TAM_LEITURA);
        if (r == -EINTR)
            continue;
        if (r < 0) {
            lia_escrever_tudo(2, "lia-hex-ref: erro de leitura ou escrita\n", lia_strlen("lia-hex-ref: erro de leitura ou escrita\n"));
            status = 1;
            break;
        }
        if (r == 0)
            break; /* EOF */
        for (long i = 0; i < r; i++) {
            linha[nlinha++] = entrada[i];
            if (nlinha == 16) {
                formatar_linha(16);
                offset += 16;
                nlinha = 0;
            }
        }
    }
    if (nlinha > 0)
        formatar_linha(nlinha);
    flush_saida();
    if (fd != 0)
        lia_close((int)fd);
    lia_exit(status);
}
