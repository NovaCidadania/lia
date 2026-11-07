/* Lia — oráculo escalar em C para lia_memcpy/lia_memset/lia_memcmp.
 * Usado só em testes/benchmark para comparar contra os backends em
 * Assembly (nucleo/x86_64/lia-mem.S); nunca linkado no binário lia.
 * Mesmo contrato documentado em docs/abi.md §6: regiões de memcpy não se
 * sobrepõem; n == 0 nunca toca memória.
 */
#include "../../runtime/lia.h"

void *lia_memcpy_ref(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *lia_memset_ref(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int lia_memcmp_ref(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return *x - *y;
        x++;
        y++;
    }
    return 0;
}
