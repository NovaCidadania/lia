/* lia-mem-bench — benchmark reproduzível de lia_memcpy/lia_memset/lia_memcmp.
 *
 * Mede cada backend (escalar, sse2, avx2 — os que a CPU suportar) e
 * tamanho (pequeno/médio/grande) separadamente, usando `rdtsc` para ciclos
 * e `lia_clock` (CLOCK_MONOTONIC) para tempo de parede. Repete N vezes e
 * reporta a mediana (não a melhor nem a média) — ver docs/abi.md §6 e
 * PLANO.md. Não é critério de aprovação/reprovação de teste: só relatório.
 */
#include "../../runtime/lia.h"

#define CLOCK_MONOTONIC 1
#define REPETICOES 9
#define CAP_MAX (4 * 1024 * 1024)

static unsigned char g_a[CAP_MAX];
static unsigned char g_b[CAP_MAX];

static Buf g_out;

static u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static i64 agora_ns(void)
{
    struct lia_timespec ts;
    lia_clock(CLOCK_MONOTONIC, &ts);
    return ts.s * 1000000000LL + ts.ns;
}

static void ordenar(u64 *v, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        u64 x = v[i];
        j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
}

static const size_t TAMANHOS[] = { 64, 4096, 1048576 };
static const char *NOME_TAM[] = { "64 B (pequeno)", "4 KiB (medio)", "1 MiB (grande)" };

typedef void *(*fn_memcpy)(void *, const void *, size_t);
typedef void *(*fn_memset)(void *, int, size_t);
typedef int (*fn_memcmp)(const void *, const void *, size_t);

static void bench_um(const char *backend, size_t tam, const char *nome_tam,
                      fn_memcpy mcpy, fn_memset mset, fn_memcmp mcmp)
{
    u64 ciclos[REPETICOES];
    i64 ns[REPETICOES];
    int iter, r;

    /* número de iterações por amostra: mais para tamanhos pequenos, para
     * que cada amostra dure o suficiente para ser medida com precisão */
    iter = (int)(tam <= 64 ? 200000 : tam <= 4096 ? 20000 : 200);

    for (r = 0; r < REPETICOES; r++) {
        i64 t0 = agora_ns();
        u64 c0 = rdtsc();
        for (int i = 0; i < iter; i++) {
            mcpy(g_b, g_a, tam);
            mset(g_a, (int)i, tam);
            mcmp(g_a, g_b, tam);
        }
        u64 c1 = rdtsc();
        i64 t1 = agora_ns();
        ciclos[r] = c1 - c0;
        ns[r] = t1 - t0;
    }

    u64 ciclos_ord[REPETICOES];
    i64 ns_ord[REPETICOES];
    for (r = 0; r < REPETICOES; r++) { ciclos_ord[r] = ciclos[r]; ns_ord[r] = ns[r]; }
    ordenar(ciclos_ord, REPETICOES);
    /* ordena ns pelo mesmo critério simples (insertion sort separado, ok
     * porque só precisamos da mediana de cada série, não do pareamento) */
    {
        u64 tmp[REPETICOES];
        for (r = 0; r < REPETICOES; r++) tmp[r] = (u64)ns_ord[r];
        ordenar(tmp, REPETICOES);
        for (r = 0; r < REPETICOES; r++) ns_ord[r] = (i64)tmp[r];
    }

    u64 ciclos_mediana = ciclos_ord[REPETICOES / 2];
    i64 ns_mediana = ns_ord[REPETICOES / 2];
    u64 bytes_totais = (u64)tam * (u64)iter * 3; /* memcpy + memset + memcmp */
    double mibps = ns_mediana > 0
        ? ((double)bytes_totais / (1024.0 * 1024.0)) / ((double)ns_mediana / 1e9)
        : 0.0;
    double ciclos_por_byte = bytes_totais > 0 ? (double)ciclos_mediana / (double)bytes_totais : 0.0;

    b_s(&g_out, "backend="); b_s(&g_out, backend);
    b_s(&g_out, "\ttamanho="); b_s(&g_out, nome_tam);
    b_s(&g_out, "\titeracoes="); b_u(&g_out, (u64)iter);
    b_s(&g_out, "\tmediana_ns="); b_u(&g_out, (u64)ns_mediana);
    b_s(&g_out, "\tMiBps="); b_u(&g_out, (u64)(mibps + 0.5));
    b_s(&g_out, "\tciclos_por_byte_x1000="); b_u(&g_out, (u64)(ciclos_por_byte * 1000.0 + 0.5));
    b_c(&g_out, '\n');
}

long lia_main(long argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    static char saida_mem[8192];
    b_init(&g_out, saida_mem, sizeof(saida_mem), 1);

    for (size_t i = 0; i < (size_t)CAP_MAX; i++)
        g_a[i] = (unsigned char)(i % 251);

    int cap = lia_mem_capacidade();

    struct { int id; const char *nome; fn_memcpy mcpy; fn_memset mset; fn_memcmp mcmp; } backs[] = {
        { LIA_MEM_ESCALAR, "escalar", lia_memcpy_escalar, lia_memset_escalar, lia_memcmp_escalar },
        { LIA_MEM_SSE2,    "sse2",    lia_memcpy_sse2,    lia_memset_sse2,    lia_memcmp_sse2 },
        { LIA_MEM_AVX2,    "avx2",    lia_memcpy_avx2,    lia_memset_avx2,    lia_memcmp_avx2 },
        { -2,              "ref_c",   lia_memcpy_ref,     lia_memset_ref,     lia_memcmp_ref },
    };

    for (size_t bi = 0; bi < sizeof(backs) / sizeof(backs[0]); bi++) {
        if (backs[bi].id == LIA_MEM_SSE2 && !(cap & 1))
            continue;
        if (backs[bi].id == LIA_MEM_AVX2 && !(cap & 2))
            continue;
        for (size_t ti = 0; ti < sizeof(TAMANHOS) / sizeof(TAMANHOS[0]); ti++)
            bench_um(backs[bi].nome, TAMANHOS[ti], NOME_TAM[ti],
                     backs[bi].mcpy, backs[bi].mset, backs[bi].mcmp);
    }

    b_flush(&g_out);
    lia_exit(0);
    return 0;
}
