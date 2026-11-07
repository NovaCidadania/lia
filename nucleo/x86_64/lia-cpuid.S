# Lia — detecção segura de capacidades de CPU (x86-64 Linux, sem libc).
#
# Nunca executa uma instrução SSE2/AVX2 sem antes confirmar, via CPUID (e,
# para AVX2, também XGETBV), que o processador e o sistema operacional
# suportam o estado vetorial correspondente. Ver docs/abi.md §6 e PLANO.md.
#
# lia_cpuid_sse2(void) -> int (0 ou 1)
#   SSE2 é parte obrigatória da arquitetura x86-64 (todo processador que roda
#   este binário já o suporta), mas o bit é checado de qualquer forma via
#   CPUID.1:EDX[26], por auditabilidade e para nunca assumir suporte sem
#   verificar.
#
# lia_cpuid_avx2(void) -> int (0 ou 1)
#   Exige, nesta ordem: CPUID.1:ECX.OSXSAVE[27] (o SO salva/restaura estado
#   estendido) e CPUID.1:ECX.AVX[28]; XGETBV com XCR0 bits 1 e 2 (SSE e AVX)
#   ligados (o SO habilitou o estado); CPUID.7.0:EBX.AVX2[5]. Só retorna 1 se
#   todas as condições forem verdadeiras — qualquer uma faltando, retorna 0
#   sem jamais executar uma instrução AVX/AVX2.

    .intel_syntax noprefix
    .text

    .globl lia_cpuid_sse2
    .type lia_cpuid_sse2, @function
lia_cpuid_sse2:
    push    rbx
    mov     eax, 1
    cpuid
    mov     eax, edx
    pop     rbx
    shr     eax, 26
    and     eax, 1
    ret
    .size lia_cpuid_sse2, . - lia_cpuid_sse2

    .globl lia_cpuid_avx2
    .type lia_cpuid_avx2, @function
lia_cpuid_avx2:
    push    rbx
    mov     eax, 1
    cpuid
    mov     r9d, ecx            # preserva leaf1.ecx através dos cpuid seguintes
    test    r9d, 1 << 27        # OSXSAVE
    jz      .Lnao
    test    r9d, 1 << 28        # AVX
    jz      .Lnao
    xor     ecx, ecx
    xgetbv                      # edx:eax = XCR0; entrada implícita ecx=0
    and     eax, 0x6            # bits 1 (SSE) e 2 (AVX)
    cmp     eax, 0x6
    jne     .Lnao
    mov     eax, 7
    xor     ecx, ecx
    cpuid
    test    ebx, 1 << 5         # AVX2
    jz      .Lnao
    mov     eax, 1
    jmp     .Lfim
.Lnao:
    xor     eax, eax
.Lfim:
    pop     rbx
    ret
    .size lia_cpuid_avx2, . - lia_cpuid_avx2

    .section .note.GNU-stack,"",@progbits
