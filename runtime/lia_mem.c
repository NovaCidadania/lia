/* Lia — seleção de backend para lia_memcpy/lia_memset/lia_memcmp.
 * Detecta capacidades da CPU uma vez (CPUID, nunca assume suporte) e
 * despacha para o melhor backend disponível: AVX2 > SSE2 > escalar.
 * Ver docs/abi.md §6.
 */
#include "lia.h"

static int g_detectado = 0;
static int g_cap = 0;             /* bit0 = SSE2, bit1 = AVX2 */
static int g_forcado = LIA_MEM_AUTO;

static void garantir_deteccao(void)
{
    if (g_detectado)
        return;
    g_cap = 0;
    if (lia_cpuid_sse2())
        g_cap |= 1;
    if (lia_cpuid_avx2())
        g_cap |= 2;
    g_detectado = 1;
}

int lia_mem_capacidade(void)
{
    garantir_deteccao();
    return g_cap;
}

int lia_mem_selecionar(int modo)
{
    garantir_deteccao();
    switch (modo) {
    case LIA_MEM_AUTO:
    case LIA_MEM_ESCALAR:
        g_forcado = modo;
        return 0;
    case LIA_MEM_SSE2:
        if (!(g_cap & 1))
            return -1;
        g_forcado = modo;
        return 0;
    case LIA_MEM_AVX2:
        if (!(g_cap & 2))
            return -1;
        g_forcado = modo;
        return 0;
    default:
        return -1;              /* backend inválido: rejeitado, nunca executado */
    }
}

int lia_mem_backend_ativo(void)
{
    garantir_deteccao();
    if (g_forcado != LIA_MEM_AUTO)
        return g_forcado;
    if (g_cap & 2)
        return LIA_MEM_AVX2;
    if (g_cap & 1)
        return LIA_MEM_SSE2;
    return LIA_MEM_ESCALAR;
}

const char *lia_mem_backend_nome(int backend)
{
    switch (backend) {
    case LIA_MEM_ESCALAR: return "escalar";
    case LIA_MEM_SSE2:    return "sse2";
    case LIA_MEM_AVX2:    return "avx2";
    default:              return "?";
    }
}

void *lia_memcpy(void *dst, const void *src, size_t n)
{
    switch (lia_mem_backend_ativo()) {
    case LIA_MEM_AVX2:   return lia_memcpy_avx2(dst, src, n);
    case LIA_MEM_SSE2:   return lia_memcpy_sse2(dst, src, n);
    default:             return lia_memcpy_escalar(dst, src, n);
    }
}

void *lia_memset(void *dst, int c, size_t n)
{
    switch (lia_mem_backend_ativo()) {
    case LIA_MEM_AVX2:   return lia_memset_avx2(dst, c, n);
    case LIA_MEM_SSE2:   return lia_memset_sse2(dst, c, n);
    default:             return lia_memset_escalar(dst, c, n);
    }
}

int lia_memcmp(const void *a, const void *b, size_t n)
{
    switch (lia_mem_backend_ativo()) {
    case LIA_MEM_AVX2:   return lia_memcmp_avx2(a, b, n);
    case LIA_MEM_SSE2:   return lia_memcmp_sse2(a, b, n);
    default:             return lia_memcmp_escalar(a, b, n);
    }
}
