/* lia-mem-teste — testes internos de lia_memcpy/lia_memset/lia_memcmp.
 *
 * Programa freestanding próprio (sem libc), ligado a entrada.o/sys.o/
 * texto.o do runtime mais nucleo/x86_64/lia-mem.S, lia-cpuid.S e o oráculo
 * nucleo/referencia/lia_mem_ref.c. Roda via `make teste` (testes/teste.sh
 * invoca o binário e reaproveita as linhas "ok "/"FALHA " no seu próprio
 * contador agregado).
 *
 * Cobre: tamanho zero; 1 byte até alguns MiB; fronteiras de alinhamento de
 * 0 a 63; padrões zero/0xff/aleatório/repetitivo; memcmp igual/menor/maior;
 * Assembly vs referência C; fronteiras dos blocos SSE2 (16) e AVX2 (32);
 * múltiplos processos; seleção manual de backend e rejeição de backend
 * inválido/indisponível (fallback escalar).
 */
#include "../../runtime/lia.h"

#define CAP (2 * 1024 * 1024 + 256)

static unsigned char g_src[CAP];
static unsigned char g_dst_asm[CAP];
static unsigned char g_dst_ref[CAP];

static long g_total = 0, g_falhas = 0;

static u64 g_rng = 0x2545F4914F6CDD1DULL;
static unsigned char prng(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (unsigned char)(g_rng & 0xff);
}

static void preencher(unsigned char *p, size_t n, int padrao)
{
    size_t i;
    switch (padrao) {
    case 0: for (i = 0; i < n; i++) p[i] = 0; break;
    case 1: for (i = 0; i < n; i++) p[i] = 0xff; break;
    case 2: for (i = 0; i < n; i++) p[i] = prng(); break;
    default: for (i = 0; i < n; i++) p[i] = (unsigned char)(i % 251); break; /* repetitivo */
    }
}

static Buf g_out;

/* Um bloco de casos (memcpy+memset+memcmp) para um backend/tamanho/
 * alinhamento/padrão. Retorna 1 se tudo bateu com o oráculo em C. */
static int caso(size_t tam, size_t desloc_src, size_t desloc_dst, int padrao)
{
    if (desloc_src + tam > CAP || desloc_dst + tam > CAP)
        return 1; /* fora do buffer: não testa, não conta como falha */

    preencher(g_src, desloc_src + tam, padrao);

    unsigned char *src = g_src + desloc_src;
    unsigned char *da = g_dst_asm + desloc_dst;
    unsigned char *dr = g_dst_ref + desloc_dst;

    /* sentinela para garantir que não escreve fora do intervalo pedido */
    g_dst_asm[CAP - 1] = 0x5a;
    g_dst_ref[CAP - 1] = 0x5a;

    lia_memcpy(da, src, tam);
    lia_memcpy_ref(dr, src, tam);
    if (lia_memcmp_ref(da, dr, tam) != 0)
        return 0;

    unsigned char c = (unsigned char)(prng());
    lia_memset(da, c, tam);
    lia_memset_ref(dr, c, tam);
    if (lia_memcmp_ref(da, dr, tam) != 0)
        return 0;

    /* memcmp: igual */
    int ra = lia_memcmp(da, dr, tam);
    int rr = lia_memcmp_ref(da, dr, tam);
    if ((ra == 0) != (rr == 0))
        return 0;

    if (tam > 0) {
        /* memcmp: menor e maior (altera o último byte de uma cópia) */
        lia_memcpy(dr, da, tam);
        if (dr[tam - 1] != 0xff) {
            dr[tam - 1] = (unsigned char)(da[tam - 1] + 1);
            int sa = lia_memcmp(da, dr, tam);
            int sr = lia_memcmp_ref(da, dr, tam);
            if ((sa < 0) != (sr < 0) || (sa == 0) != (sr == 0) || (sa > 0) != (sr > 0))
                return 0;
        }
    }

    if (g_dst_asm[CAP - 1] != 0x5a || g_dst_ref[CAP - 1] != 0x5a)
        return 0; /* escreveu além do que foi pedido */

    return 1;
}

static const size_t TAMANHOS[] = {
    0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 23, 24, 31, 32, 33, 47, 48, 63, 64, 65,
    127, 128, 129, 255, 256, 257, 511, 512, 1023, 1024, 4095, 4096, 4097,
    65535, 65536, 65537, 1048576, 2 * 1048576,
};
#define N_TAMANHOS (sizeof(TAMANHOS) / sizeof(TAMANHOS[0]))

static void testar_backend(int backend, const char *nome)
{
    size_t t, al_s, al_d;
    int padrao;
    long falhas_antes = g_falhas;
    long casos = 0;

    if (lia_mem_selecionar(backend) != 0) {
        g_total++; g_falhas++;
        b_s(&g_out, "FALHA lia-mem: nao foi possivel selecionar backend ");
        b_s(&g_out, nome);
        b_c(&g_out, '\n');
        return;
    }

    for (padrao = 0; padrao < 4; padrao++) {
        for (t = 0; t < N_TAMANHOS; t++) {
            size_t tam = TAMANHOS[t];
            if (tam > 4096) {
                /* tamanhos grandes: só alinhamento 0, para manter o tempo
                 * de execução previsível mesmo sob carga de CPU */
                casos++;
                if (!caso(tam, 0, 0, padrao))
                    g_falhas++;
                continue;
            }
            for (al_s = 0; al_s <= 63; al_s += (al_s < 16 ? 1 : 7)) {
                for (al_d = 0; al_d <= 63; al_d += (al_d < 16 ? 3 : 11)) {
                    casos++;
                    if (!caso(tam, al_s, al_d, padrao))
                        g_falhas++;
                }
            }
        }
    }

    g_total++;
    if (g_falhas == falhas_antes) {
        b_s(&g_out, "ok   lia-mem backend=");
        b_s(&g_out, nome);
        b_s(&g_out, ": ");
        b_u(&g_out, (u64)casos);
        b_s(&g_out, " casos (memcpy+memset+memcmp, 4 padroes, alinhamento 0-63, 0..2MiB)\n");
    } else {
        g_falhas++; /* conta a linha agregada como falha também */
        b_s(&g_out, "FALHA lia-mem backend=");
        b_s(&g_out, nome);
        b_s(&g_out, ": ");
        b_u(&g_out, (u64)(g_falhas - falhas_antes - 1));
        b_s(&g_out, "/");
        b_u(&g_out, (u64)casos);
        b_s(&g_out, " casos falharam\n");
    }
}

static void testar_selecao_backend(void)
{
    int cap = lia_mem_capacidade();

    g_total++;
    if (lia_mem_selecionar(99) == -1) {
        b_s(&g_out, "ok   lia-mem: backend invalido (99) rejeitado sem executar nada\n");
    } else {
        g_falhas++;
        b_s(&g_out, "FALHA lia-mem: backend invalido (99) deveria ser rejeitado\n");
    }

    if (!(cap & 2)) {
        g_total++;
        if (lia_mem_selecionar(LIA_MEM_AVX2) == -1) {
            b_s(&g_out, "ok   lia-mem: AVX2 indisponivel nesta CPU, forcar rejeitado\n");
        } else {
            g_falhas++;
            b_s(&g_out, "FALHA lia-mem: AVX2 forcado sem suporte da CPU\n");
        }
    }
    if (!(cap & 1)) {
        g_total++;
        if (lia_mem_selecionar(LIA_MEM_SSE2) == -1) {
            b_s(&g_out, "ok   lia-mem: SSE2 indisponivel nesta CPU, forcar rejeitado\n");
        } else {
            g_falhas++;
            b_s(&g_out, "FALHA lia-mem: SSE2 forcado sem suporte da CPU\n");
        }
    }

    lia_mem_selecionar(LIA_MEM_AUTO);
    g_total++;
    int ativo = lia_mem_backend_ativo();
    int esperado = (cap & 2) ? LIA_MEM_AVX2 : (cap & 1) ? LIA_MEM_SSE2 : LIA_MEM_ESCALAR;
    if (ativo == esperado) {
        b_s(&g_out, "ok   lia-mem: AUTO escolhe o melhor backend disponivel (");
        b_s(&g_out, lia_mem_backend_nome(ativo));
        b_s(&g_out, ")\n");
    } else {
        g_falhas++;
        b_s(&g_out, "FALHA lia-mem: AUTO nao escolheu o melhor backend\n");
    }
}

/* múltiplos processos: cada filho roda uma verificação independente do
 * backend escalar (sempre disponível) e sai com 0/1; o pai confere todos. */
static void testar_multiplos_processos(void)
{
    enum { N = 4 };
    long pids[N];
    int i, ok_todos = 1;

    for (i = 0; i < N; i++) {
        long pid = lia_fork();
        if (pid == 0) {
            lia_mem_selecionar(LIA_MEM_ESCALAR);
            int r = caso(4096 + (size_t)i, (size_t)i, (size_t)(i * 3), i % 4) ? 0 : 1;
            lia_exit(r);
        }
        pids[i] = pid;
    }
    for (i = 0; i < N; i++) {
        int status = 0;
        while (lia_wait4(pids[i], &status, 0) == -EINTR)
            ;
        int rc = (status & 0x7f) == 0 ? (status >> 8) & 0xff : 128 + (status & 0x7f);
        if (rc != 0)
            ok_todos = 0;
    }

    g_total++;
    if (ok_todos) {
        b_s(&g_out, "ok   lia-mem: backend escalar correto em 4 processos filhos concorrentes\n");
    } else {
        g_falhas++;
        b_s(&g_out, "FALHA lia-mem: processo filho reportou resultado incorreto\n");
    }
}

long lia_main(long argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    static char saida_mem[16384];
    b_init(&g_out, saida_mem, sizeof(saida_mem), 1);

    int cap = lia_mem_capacidade();
    b_s(&g_out, "lia-mem: capacidade da CPU = ");
    b_s(&g_out, (cap & 1) ? "sse2 " : "(sem sse2) ");
    b_s(&g_out, (cap & 2) ? "avx2" : "(sem avx2)");
    b_c(&g_out, '\n');

    testar_backend(LIA_MEM_ESCALAR, "escalar");
    if (cap & 1)
        testar_backend(LIA_MEM_SSE2, "sse2");
    if (cap & 2)
        testar_backend(LIA_MEM_AVX2, "avx2");

    testar_selecao_backend();
    testar_multiplos_processos();

    b_c(&g_out, '\n');
    b_s(&g_out, "lia-mem: "); b_u(&g_out, (u64)g_total); b_s(&g_out, " testes internos, ");
    b_u(&g_out, (u64)g_falhas); b_s(&g_out, " falhas\n");
    b_flush(&g_out);

    lia_exit(g_falhas == 0 ? 0 : 1);
    return 0;
}
