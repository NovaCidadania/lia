# Lia — primitivas de memória (x86-64 Linux, sem libc): memcpy/memset/memcmp
# em três backends (escalar, SSE2, AVX2). Ver docs/abi.md §6.
#
# Contrato (igual nos três backends, documentado em docs/abi.md):
#   lia_memcpy_*(dst, src, n) -> dst       # regiões NÃO podem se sobrepor
#   lia_memset_*(dst, c, n)   -> dst       # c é truncado para unsigned char
#   lia_memcmp_*(a, b, n)     -> int       # 0 se iguais; senão a diferença
#                                           # (com sinal) entre os primeiros
#                                           # bytes que diferem — só o sinal
#                                           # é garantido, igual ao memcmp
#                                           # de runtime/texto.c
#   n == 0 nunca lê nem escreve memória, para qualquer valor de dst/src/a/b.
#   Nenhum requisito de alinhamento: todo acesso é via mov/movdqu/vmovdqu
#   não-alinhado.
#   Registradores preservados pela convenção padrão (rbx, rbp, r12-r15)
#   nunca são tocados por estas rotinas — não precisam de push/pop.

    .intel_syntax noprefix
    .text

# ============================== escalar ====================================

    .globl lia_memcpy_escalar
    .type lia_memcpy_escalar, @function
lia_memcpy_escalar:
    mov     rax, rdi
    test    rdx, rdx
    jz      .Lce_fim
    xor     rcx, rcx
.Lce_loop:
    mov     r8b, [rsi + rcx]
    mov     [rdi + rcx], r8b
    inc     rcx
    cmp     rcx, rdx
    jne     .Lce_loop
.Lce_fim:
    ret
    .size lia_memcpy_escalar, . - lia_memcpy_escalar

    .globl lia_memset_escalar
    .type lia_memset_escalar, @function
lia_memset_escalar:
    mov     rax, rdi
    test    rdx, rdx
    jz      .Lse_fim
    mov     r8b, sil
    xor     rcx, rcx
.Lse_loop:
    mov     [rdi + rcx], r8b
    inc     rcx
    cmp     rcx, rdx
    jne     .Lse_loop
.Lse_fim:
    ret
    .size lia_memset_escalar, . - lia_memset_escalar

    .globl lia_memcmp_escalar
    .type lia_memcmp_escalar, @function
lia_memcmp_escalar:
    xor     rcx, rcx
    test    rdx, rdx
    jz      .Lme_igual
.Lme_loop:
    movzx   eax, byte ptr [rdi + rcx]
    movzx   r8d, byte ptr [rsi + rcx]
    cmp     eax, r8d
    jne     .Lme_dif
    inc     rcx
    cmp     rcx, rdx
    jne     .Lme_loop
.Lme_igual:
    xor     eax, eax
    ret
.Lme_dif:
    sub     eax, r8d
    ret
    .size lia_memcmp_escalar, . - lia_memcmp_escalar

# ================================ SSE2 ======================================
# 16 bytes por iteração (xmm, movdqu — não exige alinhamento); resto em laço
# de bytes.

    .globl lia_memcpy_sse2
    .type lia_memcpy_sse2, @function
lia_memcpy_sse2:
    mov     rax, rdi
    xor     r8, r8
    mov     rcx, rdx
    cmp     rcx, 16
    jb      .Lc2_tail
    mov     r9, rcx
    shr     r9, 4
.Lc2_loop16:
    movdqu  xmm0, [rsi + r8]
    movdqu  [rdi + r8], xmm0
    add     r8, 16
    dec     r9
    jnz     .Lc2_loop16
.Lc2_tail:
    mov     r9, rdx
    sub     r9, r8
    test    r9, r9
    jz      .Lc2_fim
    add     rsi, r8
    add     rdi, r8
    xor     r10, r10
.Lc2_tloop:
    mov     cl, [rsi + r10]
    mov     [rdi + r10], cl
    inc     r10
    cmp     r10, r9
    jne     .Lc2_tloop
.Lc2_fim:
    ret
    .size lia_memcpy_sse2, . - lia_memcpy_sse2

    .globl lia_memset_sse2
    .type lia_memset_sse2, @function
lia_memset_sse2:
    mov     rax, rdi
    movzx   r11, sil
    mov     r10, 0x0101010101010101
    imul    r11, r10            # r11 = byte replicado 8x em 64 bits
    movq    xmm0, r11
    punpcklqdq xmm0, xmm0       # xmm0 = byte replicado 16x
    xor     r8, r8
    mov     rcx, rdx
    cmp     rcx, 16
    jb      .Ls2_tail
    mov     r9, rcx
    shr     r9, 4
.Ls2_loop16:
    movdqu  [rdi + r8], xmm0
    add     r8, 16
    dec     r9
    jnz     .Ls2_loop16
.Ls2_tail:
    mov     r9, rdx
    sub     r9, r8
    test    r9, r9
    jz      .Ls2_fim
    add     rdi, r8
    xor     rcx, rcx
.Ls2_tloop:
    mov     [rdi + rcx], r11b
    inc     rcx
    cmp     rcx, r9
    jne     .Ls2_tloop
.Ls2_fim:
    ret
    .size lia_memset_sse2, . - lia_memset_sse2

    .globl lia_memcmp_sse2
    .type lia_memcmp_sse2, @function
lia_memcmp_sse2:
    xor     r8, r8
    mov     rcx, rdx
    cmp     rcx, 16
    jb      .Lm2_tail
    mov     r9, rcx
    shr     r9, 4
.Lm2_loop16:
    movdqu  xmm0, [rdi + r8]
    movdqu  xmm1, [rsi + r8]
    pcmpeqb xmm0, xmm1
    pmovmskb eax, xmm0
    cmp     eax, 0xffff
    je      .Lm2_next
    not     eax
    and     eax, 0xffff
    bsf     eax, eax            # índice do primeiro byte diferente no bloco
    add     r8, rax
    jmp     .Lm2_ponto
.Lm2_next:
    add     r8, 16
    dec     r9
    jnz     .Lm2_loop16
.Lm2_tail:
    mov     r9, rdx
    sub     r9, r8
    test    r9, r9
    jz      .Lm2_igual
    add     rdi, r8
    add     rsi, r8
    xor     rcx, rcx
.Lm2_tloop:
    movzx   eax, byte ptr [rdi + rcx]
    movzx   r10d, byte ptr [rsi + rcx]
    cmp     eax, r10d
    jne     .Lm2_dif
    inc     rcx
    cmp     rcx, r9
    jne     .Lm2_tloop
.Lm2_igual:
    xor     eax, eax
    ret
.Lm2_dif:
    sub     eax, r10d
    ret
.Lm2_ponto:
    movzx   eax, byte ptr [rdi + r8]
    movzx   r10d, byte ptr [rsi + r8]
    sub     eax, r10d
    ret
    .size lia_memcmp_sse2, . - lia_memcmp_sse2

# ================================ AVX2 ======================================
# 32 bytes por iteração (ymm, vmovdqu); resto cai para laço de bytes. Cada
# saída passa por vzeroupper (evita penalidade de transição SSE/AVX em quem
# chamar depois usando SSE legado — não é exigido pela ABI, é só boa prática
# de performance).

    .globl lia_memcpy_avx2
    .type lia_memcpy_avx2, @function
lia_memcpy_avx2:
    mov     rax, rdi
    xor     r8, r8
    mov     rcx, rdx
    cmp     rcx, 32
    jb      .Lc3_tail
    mov     r9, rcx
    shr     r9, 5
.Lc3_loop32:
    vmovdqu ymm0, [rsi + r8]
    vmovdqu [rdi + r8], ymm0
    add     r8, 32
    dec     r9
    jnz     .Lc3_loop32
.Lc3_tail:
    mov     r9, rdx
    sub     r9, r8
    test    r9, r9
    jz      .Lc3_fim
    add     rsi, r8
    add     rdi, r8
    xor     rcx, rcx
.Lc3_tloop:
    mov     r10b, [rsi + rcx]
    mov     [rdi + rcx], r10b
    inc     rcx
    cmp     rcx, r9
    jne     .Lc3_tloop
.Lc3_fim:
    vzeroupper
    ret
    .size lia_memcpy_avx2, . - lia_memcpy_avx2

    .globl lia_memset_avx2
    .type lia_memset_avx2, @function
lia_memset_avx2:
    mov     rax, rdi
    movzx   r11, sil
    mov     r10, 0x0101010101010101
    imul    r11, r10
    movq    xmm0, r11
    vpbroadcastq ymm0, xmm0
    xor     r8, r8
    mov     rcx, rdx
    cmp     rcx, 32
    jb      .Ls3_tail
    mov     r9, rcx
    shr     r9, 5
.Ls3_loop32:
    vmovdqu [rdi + r8], ymm0
    add     r8, 32
    dec     r9
    jnz     .Ls3_loop32
.Ls3_tail:
    mov     r9, rdx
    sub     r9, r8
    test    r9, r9
    jz      .Ls3_fim
    add     rdi, r8
    xor     rcx, rcx
.Ls3_tloop:
    mov     [rdi + rcx], r11b
    inc     rcx
    cmp     rcx, r9
    jne     .Ls3_tloop
.Ls3_fim:
    vzeroupper
    ret
    .size lia_memset_avx2, . - lia_memset_avx2

    .globl lia_memcmp_avx2
    .type lia_memcmp_avx2, @function
lia_memcmp_avx2:
    xor     r8, r8
    mov     rcx, rdx
    cmp     rcx, 32
    jb      .Lm3_tail
    mov     r9, rcx
    shr     r9, 5
.Lm3_loop32:
    vmovdqu ymm0, [rdi + r8]
    vmovdqu ymm1, [rsi + r8]
    vpcmpeqb ymm2, ymm0, ymm1
    vpmovmskb eax, ymm2
    cmp     eax, 0xffffffff
    je      .Lm3_next
    not     eax
    bsf     eax, eax
    add     r8, rax
    jmp     .Lm3_ponto
.Lm3_next:
    add     r8, 32
    dec     r9
    jnz     .Lm3_loop32
.Lm3_tail:
    mov     r9, rdx
    sub     r9, r8
    test    r9, r9
    jz      .Lm3_igual
    add     rdi, r8
    add     rsi, r8
    xor     rcx, rcx
.Lm3_tloop:
    movzx   eax, byte ptr [rdi + rcx]
    movzx   r10d, byte ptr [rsi + rcx]
    cmp     eax, r10d
    jne     .Lm3_dif
    inc     rcx
    cmp     rcx, r9
    jne     .Lm3_tloop
.Lm3_igual:
    xor     eax, eax
    vzeroupper
    ret
.Lm3_dif:
    sub     eax, r10d
    vzeroupper
    ret
.Lm3_ponto:
    movzx   eax, byte ptr [rdi + r8]
    movzx   r10d, byte ptr [rsi + r8]
    sub     eax, r10d
    vzeroupper
    ret
    .size lia_memcmp_avx2, . - lia_memcmp_avx2

    .section .note.GNU-stack,"",@progbits
