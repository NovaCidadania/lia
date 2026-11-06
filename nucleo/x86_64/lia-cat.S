# lia-cat — copia arquivos (ou stdin) para stdout, em Assembly puro, sem libc.
#
#   lia-cat              : stdin -> stdout até EOF
#   lia-cat A B ...      : cada arquivo em ordem; "-" significa stdin
#
# Trata leitura/escrita parcial e EINTR. Falha em um arquivo não interrompe os
# demais; o código de saída é 1 se qualquer erro ocorreu.
#
# Registradores (callee-saved por convenção interna deste programa):
#   r12 = ponteiro em argv   r13 = argumentos restantes
#   r14 = descritor atual    r15 = status de saída

    .intel_syntax noprefix

    .section .rodata
msg_abrir:  .ascii "lia-cat: nao foi possivel abrir: "
msg_abrir_n = . - msg_abrir
msg_io:     .ascii "lia-cat: erro de leitura ou escrita\n"
msg_io_n = . - msg_io
nl:         .ascii "\n"

    .bss
    .align 64
buf:        .skip 65536

    .text
    .globl _start
_start:
    mov     r13, [rsp]              # argc
    lea     r12, [rsp + 8]          # argv
    xor     r15d, r15d
    cmp     r13, 1
    jg      .com_args
    xor     r14d, r14d              # sem argumentos: stdin
    call    copiar
    jmp     .fim
.com_args:
    dec     r13
    add     r12, 8                  # pula argv[0]
.prox:
    test    r13, r13
    jz      .fim
    mov     rsi, [r12]
    cmp     byte ptr [rsi], 45      # '-'
    jne     .abrir
    cmp     byte ptr [rsi + 1], 0
    jne     .abrir
    xor     r14d, r14d              # "-" = stdin
    call    copiar
    jmp     .avanca
.abrir:
    mov     eax, 257                # openat
    mov     rdi, -100               # AT_FDCWD
    xor     edx, edx                # O_RDONLY
    xor     r10d, r10d
    syscall
    test    rax, rax
    js      .erro_abrir
    mov     r14, rax
    call    copiar
    mov     eax, 3                  # close
    mov     rdi, r14
    syscall
.avanca:
    add     r12, 8
    dec     r13
    jmp     .prox
.erro_abrir:
    mov     r15d, 1
    lea     rsi, [rip + msg_abrir]
    mov     edx, msg_abrir_n
    call    escreve_err
    mov     rsi, [r12]              # nome do arquivo
    xor     edx, edx
.len:
    cmp     byte ptr [rsi + rdx], 0
    je      .len_ok
    inc     rdx
    jmp     .len
.len_ok:
    call    escreve_err
    lea     rsi, [rip + nl]
    mov     edx, 1
    call    escreve_err
    jmp     .avanca
.fim:
    mov     edi, r15d
    mov     eax, 231
    syscall

# escreve_err(rsi=ptr, rdx=len) -> stderr, melhor esforço
escreve_err:
    mov     eax, 1
    mov     edi, 2
    syscall
    ret

# copiar: r14 -> stdout até EOF. Em erro: msg + r15=1
copiar:
.ler:
    xor     eax, eax                # read
    mov     rdi, r14
    lea     rsi, [rip + buf]
    mov     edx, 65536
    syscall
    cmp     rax, -4                 # EINTR
    je      .ler
    test    rax, rax
    js      .err
    jz      .ok                     # EOF
    mov     rbx, rax                # bytes pendentes
    lea     r8, [rip + buf]
.esc:
    mov     eax, 1                  # write
    mov     edi, 1
    mov     rsi, r8
    mov     rdx, rbx
    syscall
    cmp     rax, -4
    je      .esc
    test    rax, rax
    jle     .err                    # erro ou 0 bytes: aborta
    add     r8, rax
    sub     rbx, rax
    jnz     .esc
    jmp     .ler
.err:
    mov     r15d, 1
    lea     rsi, [rip + msg_io]
    mov     edx, msg_io_n
    call    escreve_err
.ok:
    ret

    .section .note.GNU-stack,"",@progbits
