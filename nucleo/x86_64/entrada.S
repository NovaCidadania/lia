# Lia — núcleo x86-64 Linux
# Ponto de entrada (_start) e a única porta para o kernel (lia_syscall).
# Sem libc.
#
# _start: entrega (argc, argv, envp) a lia_main(long, char **, char **).
#
# lia_syscall(n, a1, a2, a3, a4, a5, a6)
#   entrada : rdi=n rsi..r9 = a1..a5, a6 na pilha (convenção System V)
#   saída   : rax = resultado bruto do kernel (negativo = -errno)
#   destrói : rax, rcx, r11 (como o próprio syscall), r10 (usado como a4)

    .intel_syntax noprefix
    .text

    .globl _start
_start:
    xor     ebp, ebp
    mov     rdi, [rsp]              # argc
    lea     rsi, [rsp + 8]          # argv
    lea     rdx, [rsi + rdi*8 + 8]  # envp
    and     rsp, -16
    call    lia_main
    mov     edi, eax
    mov     eax, 231                # exit_group
    syscall

    .globl lia_syscall
    .type lia_syscall, @function
lia_syscall:
    mov     rax, rdi
    mov     rdi, rsi
    mov     rsi, rdx
    mov     rdx, rcx
    mov     r10, r8
    mov     r8, r9
    mov     r9, [rsp + 8]
    syscall
    ret
    .size lia_syscall, . - lia_syscall

    .section .note.GNU-stack,"",@progbits
