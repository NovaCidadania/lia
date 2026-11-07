# lia-tipo — identifica o tipo de um arquivo pelos bytes mágicos, nunca pela
# extensão. Assembly puro, sem libc.
#
#   lia-tipo               : lê stdin
#   lia-tipo ARQUIVO        : lê ARQUIVO ("-" significa stdin)
#   lia-tipo --json [ARQUIVO]
#
# Só lê um prefixo (até 32 bytes) do início do arquivo — o suficiente para
# qualquer assinatura deste subconjunto (a mais longa, WAV, precisa dos
# primeiros 12 bytes). Um arquivo mais curto que uma assinatura simplesmente
# não casa com ela (nunca lê além do que existe, nunca assume o que não foi
# lido): ver a seção "falha segura" no comentário de detectar_tipo.
#
# Tipos reconhecidos (nesta ordem de verificação): ELF, PNG, JPEG, ZIP,
# GZIP, PDF, WAV, TEXTO (heurística: todo o prefixo lido é ASCII imprimível
# ou TAB/LF/CR), VAZIO (arquivo com zero bytes) e DESCONHECIDO (qualquer
# outra coisa — nunca um erro silencioso, é um resultado informativo).
#
# Saída humana: o nome do tipo e uma linha em branco. --json: {"tipo":".."}.
# Código de saída: 0 (tipo identificado, incluindo VAZIO/DESCONHECIDO);
# 1 (não foi possível abrir ou ler); 2 (uso incorreto).

    .intel_syntax noprefix

    .section .rodata
sig_elf:   .byte 0x7f, 'E', 'L', 'F'
sig_png:   .byte 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
sig_jpeg:  .byte 0xff, 0xd8, 0xff
sig_zip1:  .byte 'P', 'K', 0x03, 0x04
sig_zip2:  .byte 'P', 'K', 0x05, 0x06
sig_zip3:  .byte 'P', 'K', 0x07, 0x08
sig_gzip:  .byte 0x1f, 0x8b
sig_pdf:   .ascii "%PDF-"
sig_riff:  .ascii "RIFF"
sig_wave:  .ascii "WAVE"

nome_elf:   .ascii "ELF\n"
nome_png:   .ascii "PNG\n"
nome_jpeg:  .ascii "JPEG\n"
nome_zip:   .ascii "ZIP\n"
nome_gzip:  .ascii "GZIP\n"
nome_pdf:   .ascii "PDF\n"
nome_wav:   .ascii "WAV\n"
nome_texto: .ascii "TEXTO\n"
nome_vazio: .ascii "VAZIO\n"
nome_desc:  .ascii "DESCONHECIDO\n"

opt_json:   .asciz "--json"

json_pre:   .ascii "{\"tipo\":\""
json_pre_n = . - json_pre
json_suf:   .ascii "\"}\n"
json_suf_n = . - json_suf

msg_abrir:  .ascii "lia-tipo: nao foi possivel abrir: "
msg_abrir_n = . - msg_abrir
msg_io:     .ascii "lia-tipo: erro de leitura\n"
msg_io_n = . - msg_io
msg_uso:    .ascii "uso: lia-tipo [--json] [arquivo]\n"
msg_uso_n = . - msg_uso
newline:    .ascii "\n"

    .bss
    .align 16
buf:        .skip 32        # prefixo lido do arquivo (sniff)
g_n:        .quad 0         # bytes realmente lidos em 'buf' (0..32)

    .text
    .globl _start
_start:
    mov     r13, [rsp]              # argc
    xor     r12d, r12d              # r12 = 1 se --json
    xor     r14d, r14d              # r14 = ponteiro do nome do arquivo (0 = stdin)
    mov     r15, 1                  # r15 = índice de argv em percurso

    dec     r13                     # argumentos (sem argv[0])
.prox_arg:
    test    r13, r13
    jz      .args_prontos
    mov     rsi, [rsp + 8 + r15 * 8]
    mov     rbx, rsi                # guarda o ponteiro original: strcmp_simples
                                     # avança rsi conforme compara (bug encontrado
                                     # e corrigido durante a validação real — "mov
                                     # r14, rsi" direto do retorno pegava o
                                     # ponteiro já andado, não o início da string)
    lea     rdi, [rip + opt_json]
    call    strcmp_simples
    test    eax, eax
    jnz     .nao_e_json
    mov     r12d, 1
    jmp     .arg_consumido
.nao_e_json:
    test    r14, r14
    jnz     .erro_uso               # um nome já visto: um segundo é demais
    mov     r14, rbx
.arg_consumido:
    inc     r15
    dec     r13
    jmp     .prox_arg
.args_prontos:
    test    r14, r14
    jz      .usa_stdin
    cmp     byte ptr [r14], 45      # '-'
    jne     .abrir_arg
    cmp     byte ptr [r14 + 1], 0
    jne     .abrir_arg
    jmp     .usa_stdin
.abrir_arg:
    mov     eax, 257                # openat(AT_FDCWD, r14, O_RDONLY, 0)
    mov     rdi, -100
    mov     rsi, r14
    xor     edx, edx
    xor     r10d, r10d
    syscall
    test    rax, rax
    js      .erro_abrir
    mov     r13, rax                # r13 = descritor (reaproveitado; argc já não importa)
    jmp     .ler
.usa_stdin:
    xor     r13d, r13d
.ler:
    # lê até enchermos 'buf' (32 bytes) ou EOF — leitura parcial tratada em
    # laço, igual aos outros binários do núcleo.
    xor     ebx, ebx                # bytes já acumulados em buf
.ler_mais:
    cmp     rbx, 32
    jge     .ler_pronto
    xor     eax, eax
    mov     rdi, r13
    lea     rsi, [rip + buf]
    add     rsi, rbx
    mov     rdx, 32
    sub     rdx, rbx
    syscall
    cmp     rax, -4                 # EINTR
    je      .ler_mais
    test    rax, rax
    js      .erro_leitura
    jz      .ler_pronto             # EOF
    add     rbx, rax
    jmp     .ler_mais
.ler_pronto:
    mov     [rip + g_n], rbx
    test    r14, r14
    jz      .detectar
    test    r13, r13
    jz      .detectar
    mov     eax, 3                  # close (melhor esforço)
    mov     rdi, r13
    syscall
.detectar:
    call    detectar_tipo           # rax = ponteiro para nome_* (com \n)
    mov     r13, rax
    test    r12d, r12d
    jnz     .emitir_json
    mov     rsi, r13
    xor     edx, edx
.len_nome_tipo:
    cmp     byte ptr [rsi + rdx], 0x0a
    je      .tem_len
    inc     rdx
    jmp     .len_nome_tipo
.tem_len:
    inc     rdx                     # inclui o '\n'
    call    escreve_out
    jmp     .fim_ok
.emitir_json:
    lea     rsi, [rip + json_pre]
    mov     edx, json_pre_n
    call    escreve_out
    mov     rsi, r13
    xor     edx, edx
.len_nome_tipo_j:
    cmp     byte ptr [rsi + rdx], 0x0a
    je      .tem_len_j
    inc     rdx
    jmp     .len_nome_tipo_j
.tem_len_j:
    call    escreve_out
    lea     rsi, [rip + json_suf]
    mov     edx, json_suf_n
    call    escreve_out
.fim_ok:
    xor     edi, edi
    jmp     .saida
.erro_uso:
    lea     rsi, [rip + msg_uso]
    mov     edx, msg_uso_n
    call    escreve_err
    mov     edi, 2
    jmp     .saida
.erro_abrir:
    lea     rsi, [rip + msg_abrir]
    mov     edx, msg_abrir_n
    call    escreve_err
    mov     rsi, r14
    xor     edx, edx
.len_nome_arq:
    cmp     byte ptr [rsi + rdx], 0
    je      .len_arq_ok
    inc     rdx
    jmp     .len_nome_arq
.len_arq_ok:
    call    escreve_err
    lea     rsi, [rip + newline]
    mov     edx, 1
    call    escreve_err
    mov     edi, 1
    jmp     .saida
.erro_leitura:
    lea     rsi, [rip + msg_io]
    mov     edx, msg_io_n
    call    escreve_err
    mov     edi, 1
.saida:
    mov     eax, 231
    syscall

# strcmp_simples(rdi=a asciz, rsi=b, sem comprimento) -> eax=0 se iguais.
# Só usado para comparar argv[i] com a opção fixa "--json" (ambas vêm de
# strings terminadas em 0: argv por convenção do kernel, opt_json por .asciz).
strcmp_simples:
.sc_loop:
    movzx   eax, byte ptr [rdi]
    movzx   ecx, byte ptr [rsi]
    cmp     al, cl
    jne     .sc_dif
    test    al, al
    jz      .sc_igual
    inc     rdi
    inc     rsi
    jmp     .sc_loop
.sc_dif:
    mov     eax, 1
    ret
.sc_igual:
    xor     eax, eax
    ret

# escreve_out(rsi=ptr, rdx=len) -> stdout, melhor esforço (saída curta e
# única por execução: não há por que complicar com buffer/flush aqui).
escreve_out:
    mov     eax, 1
    mov     edi, 1
    syscall
    ret

# escreve_err(rsi=ptr, rdx=len) -> stderr, melhor esforço.
escreve_err:
    mov     eax, 1
    mov     edi, 2
    syscall
    ret

# casa(rdi=buf, rsi=n, rdx=offset, rcx=sig_ptr, r8=sig_len) -> rax=1 se os
# sig_len bytes de buf a partir de offset são iguais a sig_ptr; rax=0 se
# não couberem em n ou se algum byte diferir. Nunca lê buf fora de [0,n).
casa:
    mov     rax, rdx
    add     rax, r8
    cmp     rax, rsi
    jg      .casa_nao
    lea     r11, [rdi + rdx]            # endereço base+offset (um só cálculo;
                                           # x86-64 não permite rdi+rdx+r9 direto,
                                           # três registradores não cabem num só
                                           # operando de memória)
    xor     r9d, r9d
.casa_byte:
    cmp     r9, r8
    jge     .casa_sim
    movzx   eax, byte ptr [r11 + r9]
    movzx   r10d, byte ptr [rcx + r9]   # NUNCA usar rcx/ecx aqui: destruiria o
                                        # ponteiro da assinatura (bug encontrado
                                        # e corrigido durante a validação real)
    cmp     al, r10b
    jne     .casa_nao
    inc     r9
    jmp     .casa_byte
.casa_sim:
    mov     eax, 1
    ret
.casa_nao:
    xor     eax, eax
    ret

# detectar_tipo() -> rax = ponteiro para a string nome_* (termina em '\n')
# correspondente ao tipo do prefixo em 'buf' (g_n bytes válidos).
#
# Falha segura: cada assinatura só "casa" se g_n for grande o bastante para
# contê-la por completo (ver 'casa' acima) — um arquivo truncado ou vazio
# nunca é confundido com um tipo que exigiria mais bytes do que existem;
# ele cai para a heurística de texto ou para DESCONHECIDO.
detectar_tipo:
    push    r12
    push    r13
    mov     r12, [rip + g_n]
    lea     r13, [rip + buf]

    mov     rdi, r13
    mov     rsi, r12
    xor     edx, edx
    lea     rcx, [rip + sig_elf]
    mov     r8, 4
    call    casa
    test    eax, eax
    jz      .dt_png
    lea     rax, [rip + nome_elf]
    jmp     .dt_fim

.dt_png:
    mov     rdi, r13
    mov     rsi, r12
    xor     edx, edx
    lea     rcx, [rip + sig_png]
    mov     r8, 8
    call    casa
    test    eax, eax
    jz      .dt_jpeg
    lea     rax, [rip + nome_png]
    jmp     .dt_fim

.dt_jpeg:
    mov     rdi, r13
    mov     rsi, r12
    xor     edx, edx
    lea     rcx, [rip + sig_jpeg]
    mov     r8, 3
    call    casa
    test    eax, eax
    jz      .dt_zip
    lea     rax, [rip + nome_jpeg]
    jmp     .dt_fim

.dt_zip:
    mov     rdi, r13
    mov     rsi, r12
    xor     edx, edx
    lea     rcx, [rip + sig_zip1]
    mov     r8, 4
    call    casa
    test    eax, eax
    jnz     .dt_zip_sim
    mov     rdi, r13
    mov     rsi, r12
    xor     edx, edx
    lea     rcx, [rip + sig_zip2]
    mov     r8, 4
    call    casa
    test    eax, eax
    jnz     .dt_zip_sim
    mov     rdi, r13
    mov     rsi, r12
    xor     edx, edx
    lea     rcx, [rip + sig_zip3]
    mov     r8, 4
    call    casa
    test    eax, eax
    jz      .dt_gzip
.dt_zip_sim:
    lea     rax, [rip + nome_zip]
    jmp     .dt_fim

.dt_gzip:
    mov     rdi, r13
    mov     rsi, r12
    xor     edx, edx
    lea     rcx, [rip + sig_gzip]
    mov     r8, 2
    call    casa
    test    eax, eax
    jz      .dt_pdf
    lea     rax, [rip + nome_gzip]
    jmp     .dt_fim

.dt_pdf:
    mov     rdi, r13
    mov     rsi, r12
    xor     edx, edx
    lea     rcx, [rip + sig_pdf]
    mov     r8, 5
    call    casa
    test    eax, eax
    jz      .dt_wav
    lea     rax, [rip + nome_pdf]
    jmp     .dt_fim

.dt_wav:
    mov     rdi, r13
    mov     rsi, r12
    xor     edx, edx
    lea     rcx, [rip + sig_riff]
    mov     r8, 4
    call    casa
    test    eax, eax
    jz      .dt_vazio
    mov     rdi, r13
    mov     rsi, r12
    mov     rdx, 8
    lea     rcx, [rip + sig_wave]
    mov     r8, 4
    call    casa
    test    eax, eax
    jz      .dt_vazio
    lea     rax, [rip + nome_wav]
    jmp     .dt_fim

.dt_vazio:
    test    r12, r12
    jnz     .dt_texto
    lea     rax, [rip + nome_vazio]
    jmp     .dt_fim

.dt_texto:
    # heurística: todo o prefixo lido precisa ser TAB/LF/CR ou 0x20..0x7e.
    xor     r9d, r9d
.dt_texto_byte:
    cmp     r9, r12
    jge     .dt_e_texto
    movzx   eax, byte ptr [r13 + r9]
    cmp     al, 9
    je      .dt_texto_ok_byte
    cmp     al, 10
    je      .dt_texto_ok_byte
    cmp     al, 13
    je      .dt_texto_ok_byte
    cmp     al, 0x20
    jb      .dt_e_desconhecido
    cmp     al, 0x7e
    ja      .dt_e_desconhecido
.dt_texto_ok_byte:
    inc     r9
    jmp     .dt_texto_byte
.dt_e_texto:
    lea     rax, [rip + nome_texto]
    jmp     .dt_fim
.dt_e_desconhecido:
    lea     rax, [rip + nome_desc]
.dt_fim:
    pop     r13
    pop     r12
    ret

    .section .note.GNU-stack,"",@progbits
