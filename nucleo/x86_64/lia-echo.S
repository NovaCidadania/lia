# lia-echo — copia stdin para stdout, byte a byte conforme lido, sem libc.
#
# O programa mais simples possível do núcleo: prova que read/write/EOF/EINTR
# e leitura/escrita parcial funcionam, sem a complexidade de abrir arquivo
# (isso é lia-cat). Usa um buffer pequeno (4 KiB) de propósito, para que
# qualquer entrada maior que o buffer exercite várias iterações do laço.
#
# Uso: lia-echo < entrada > saida
#      lia-echo (sem argumentos; argumentos extras são ignorados)
#
# Saída: 0 em sucesso (inclusive entrada vazia); 1 se read ou write falhar.

    .intel_syntax noprefix

    .section .rodata
msg_io: .ascii "lia-echo: erro de leitura ou escrita\n"
msg_io_n = . - msg_io

    .bss
    .align 64
buf:    .skip 4096

    .text
    .globl _start
_start:
.ler:
    xor     eax, eax                # read(0, buf, 4096)
    xor     edi, edi
    lea     rsi, [rip + buf]
    mov     edx, 4096
    syscall
    cmp     rax, -4                 # EINTR: tenta de novo
    je      .ler
    test    rax, rax
    js      .erro                   # erro (< 0, != EINTR)
    jz      .fim_ok                 # EOF (== 0)
    mov     rbx, rax                # rbx = bytes pendentes para escrever
    lea     r12, [rip + buf]        # r12 = ponteiro de escrita atual
.escrever:
    mov     eax, 1                  # write(1, r12, rbx)
    mov     edi, 1
    mov     rsi, r12
    mov     rdx, rbx
    syscall
    cmp     rax, -4                 # EINTR: tenta de novo
    je      .escrever
    test    rax, rax
    jle     .erro                   # erro ou write() == 0: nunca deveria; aborta
    add     r12, rax                # avança o que já foi escrito
    sub     rbx, rax                # diminui o que falta
    jnz     .escrever               # escrita parcial: continua até esvaziar
    jmp     .ler                    # buffer todo escrito: lê o próximo bloco
.erro:
    mov     eax, 1
    mov     edi, 2                  # stderr
    lea     rsi, [rip + msg_io]
    mov     edx, msg_io_n
    syscall
    mov     edi, 1
    jmp     .saida
.fim_ok:
    xor     edi, edi
.saida:
    mov     eax, 231                # exit_group
    syscall

    .section .note.GNU-stack,"",@progbits
