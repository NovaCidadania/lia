# lia-hex — despejo hexadecimal + ASCII, em Assembly puro, sem libc.
#
#   lia-hex              : stdin -> stdout
#   lia-hex ARQUIVO       : lê ARQUIVO ("-" significa stdin)
#
# Formato de saída idêntico ao de `xxd` (sem opções) para o subconjunto
# documentado: offset em hexadecimal (mínimo 8 dígitos, crescendo se o
# arquivo passar de 4 GiB), ":", campo hexadecimal de exatamente 40 colunas
# (8 grupos de 2 bytes, um espaço antes de cada grupo, sem preencher um
# grupo incompleto — o campo todo é completado com espaços até 40 colunas),
# duas colunas de espaço e a coluna ASCII (bytes 0x20..0x7e literais, o
# resto como '.'). Esse formato foi conferido byte a byte contra o `xxd`
# desta máquina (ver testes/teste.sh) antes de ser fixado aqui.
#
# Igual a lia-cat/lia-echo: leitura em laço até EOF, EINTR tratado, escrita
# parcial tratada. A montagem de cada linha de texto acontece num buffer de
# saída (saida) que só é de fato escrito (write) quando enche ou no fim —
# isso evita um write() por campo.
#
# Convenção interna das sub-rotinas abaixo (não exportadas, por isso sem
# prefixo lia_ — ver docs/abi.md §4): seguem a ABI System V comum, exceto
# quando documentado. Nenhuma delas preserva rax/rcx/rdx/rdi/rsi por conta
# do chamador (são caller-saved normais); o chamador que precisar de um
# valor depois de uma chamada o recalcula ou o mantém num registrador que
# a sub-rotina não toca (anotado em cada uma).
#
# Código de saída: 0 em sucesso; 1 se abrir, ler ou escrever falhar.

    .intel_syntax noprefix

    .section .rodata
hexdig:      .ascii "0123456789abcdef"
msg_abrir:   .ascii "lia-hex: nao foi possivel abrir: "
msg_abrir_n = . - msg_abrir
msg_io:      .ascii "lia-hex: erro de leitura ou escrita\n"
msg_io_n = . - msg_io
msg_uso:     .ascii "uso: lia-hex [arquivo]\n"
msg_uso_n = . - msg_uso
newline:     .ascii "\n"

    .bss
    .align 64
entrada:     .skip 4096      # bloco bruto lido do arquivo/stdin
linha:       .skip 16        # até 16 bytes da linha em montagem
saida:       .skip 8192      # texto pronto, escrito em bloco
g_offset:    .quad 0         # posição (em bytes) do início da linha atual
g_nlinha:    .quad 0         # bytes já acumulados em 'linha' (0..16)
g_saida_n:   .quad 0         # bytes já acumulados em 'saida'
g_status:    .quad 0         # 0 = ok; 1 = algum erro ocorreu

    .text
    .globl _start
_start:
    mov     r13, [rsp]              # argc
    cmp     r13, 3
    jl      .argc_ok
    lea     rsi, [rip + msg_uso]
    mov     edx, msg_uso_n
    call    escreve_err
    mov     edi, 2
    jmp     .saida_final
.argc_ok:
    cmp     r13, 2
    jl      .usa_stdin
    mov     rsi, [rsp + 16]         # argv[1]
    cmp     byte ptr [rsi], 45      # '-'
    jne     .abrir_arg
    cmp     byte ptr [rsi + 1], 0
    je      .usa_stdin
.abrir_arg:
    mov     r14, rsi                # guarda o nome para a mensagem de erro
    mov     eax, 257                # openat(AT_FDCWD, nome, O_RDONLY, 0)
    mov     rdi, -100
    xor     edx, edx
    xor     r10d, r10d
    syscall
    test    rax, rax
    js      .erro_abrir
    mov     r12, rax                # r12 = descritor de entrada
    jmp     .laco
.usa_stdin:
    xor     r12d, r12d
.laco:
    xor     eax, eax                # read(r12, entrada, 4096)
    mov     rdi, r12
    lea     rsi, [rip + entrada]
    mov     edx, 4096
    syscall
    cmp     rax, -4                 # EINTR
    je      .laco
    test    rax, rax
    js      .erro_io
    jz      .eof
    mov     rbx, rax                # bytes lidos neste bloco (rbx: só usado
                                     # aqui em _start, nenhuma sub-rotina toca)
    lea     r15, [rip + entrada]    # cursor de leitura no bloco
.consumir:
    test    rbx, rbx
    jz      .laco                   # bloco todo absorvido: lê o próximo
    movzx   eax, byte ptr [r15]
    mov     rcx, [rip + g_nlinha]
    lea     rdx, [rip + linha]
    mov     byte ptr [rdx + rcx], al
    inc     rcx
    mov     [rip + g_nlinha], rcx
    inc     r15
    dec     rbx
    cmp     rcx, 16
    jl      .consumir
    call    formatar_linha          # não toca rbx/r15 (só usa r12-r14 e rsp)
    mov     rax, [rip + g_offset]
    add     rax, 16
    mov     [rip + g_offset], rax
    mov     qword ptr [rip + g_nlinha], 0
    jmp     .consumir
.eof:
    mov     rax, [rip + g_nlinha]
    test    rax, rax
    jz      .fim
    call    formatar_linha
    jmp     .fim
.erro_abrir:
    mov     qword ptr [rip + g_status], 1
    lea     rsi, [rip + msg_abrir]
    mov     edx, msg_abrir_n
    call    escreve_err
    mov     rsi, r14
    xor     edx, edx
.len_nome:
    cmp     byte ptr [rsi + rdx], 0
    je      .len_ok
    inc     rdx
    jmp     .len_nome
.len_ok:
    call    escreve_err
    lea     rsi, [rip + newline]
    mov     edx, 1
    call    escreve_err
    jmp     .fim
.erro_io:
    mov     qword ptr [rip + g_status], 1
    lea     rsi, [rip + msg_io]
    mov     edx, msg_io_n
    call    escreve_err
.fim:
    call    flush_saida
    mov     rdi, [rip + g_status]
.saida_final:
    mov     eax, 231                # exit_group
    syscall

# escreve_err(rsi=ptr, rdx=len) -> stderr, melhor esforço (sem retry: é só a
# mensagem final de erro, antes de sair). Destrói rax, rdi.
escreve_err:
    mov     eax, 1
    mov     edi, 2
    syscall
    ret

# escrever_tudo(rdi=fd, rsi=ptr, rdx=len) -> escreve len bytes por completo,
# tratando EINTR e escrita parcial. Em erro real, marca g_status=1 e retorna.
# Preserva rbx e r8 (usa-os, mas com push/pop) para poder ser chamada de
# qualquer lugar sem surpresa; destrói rax, rcx, rdx, rsi, rdi, r11.
escrever_tudo:
    push    rbx
    push    r8
    mov     rbx, rdx                # bytes restantes
    mov     r8, rsi                 # cursor
.et_escrever:
    test    rbx, rbx
    jz      .et_fim
    mov     eax, 1
    mov     rsi, r8
    mov     rdx, rbx
    syscall
    cmp     rax, -4
    je      .et_escrever
    test    rax, rax
    jg      .et_avanca
    mov     qword ptr [rip + g_status], 1
    jmp     .et_fim
.et_avanca:
    add     r8, rax
    sub     rbx, rax
    jmp     .et_escrever
.et_fim:
    pop     r8
    pop     rbx
    ret

# flush_saida() -> escreve g_saida_n bytes de 'saida' em stdout e zera o
# contador. Não toca r12-r15 (só os registradores "comuns" via escrever_tudo).
flush_saida:
    mov     rax, [rip + g_saida_n]
    test    rax, rax
    jz      .fs_fim
    mov     edi, 1
    lea     rsi, [rip + saida]
    mov     rdx, rax
    call    escrever_tudo
    mov     qword ptr [rip + g_saida_n], 0
.fs_fim:
    ret

# emit_buf(rsi=ptr, rdx=len) -> copia len bytes para saida[g_saida_n] e
# avança o contador. Quem chama já garantiu espaço (uma linha cabe folgada
# em 8 KiB — formatar_linha confere isso uma vez por linha). Destrói
# rax, rcx, rdx, rsi, rdi.
emit_buf:
    mov     rcx, [rip + g_saida_n]
    lea     rdi, [rip + saida]
    add     rdi, rcx
    add     rcx, rdx
    mov     [rip + g_saida_n], rcx
.eb_cp:
    test    rdx, rdx
    jz      .eb_fim
    mov     al, [rsi]
    mov     [rdi], al
    inc     rsi
    inc     rdi
    dec     rdx
    jmp     .eb_cp
.eb_fim:
    ret

# emit_byte(al) -> acrescenta um byte a saida[g_saida_n]. Destrói rax, rcx,
# rdi. Não toca rsi/rdx/r12-r15 — quem chama em laço (ex.: a coluna ASCII em
# formatar_linha) pode manter o cursor/contador nesses registradores.
emit_byte:
    mov     rcx, [rip + g_saida_n]
    lea     rdi, [rip + saida]
    mov     [rdi + rcx], al
    inc     rcx
    mov     [rip + g_saida_n], rcx
    ret

# hex2(rdi = byte 0..255) -> emite 2 dígitos hexadecimais minúsculos.
# Destrói rax, rcx, rdx, rdi, r8 (usa r8 para guardar o byte original,
# porque emit_byte destrói rdi).
hex2:
    mov     r8, rdi
    mov     eax, r8d
    shr     al, 4
    and     eax, 0xf
    lea     rdx, [rip + hexdig]
    movzx   eax, byte ptr [rdx + rax]
    call    emit_byte
    mov     eax, r8d
    and     eax, 0xf
    lea     rdx, [rip + hexdig]
    movzx   eax, byte ptr [rdx + rax]
    call    emit_byte
    ret

# escrever_offset(rdi = valor u64) -> emite o offset em hex, mínimo 8
# dígitos (cresce sozinho além disso, como o xxd, se o valor não couber).
# Destrói rax, rcx, rdx, rdi, rsi, r9.
escrever_offset:
    sub     rsp, 24
    mov     rax, rdi
    mov     rcx, 15                 # índice do nibble menos significativo
.eo_nibble:
    mov     rdx, rax
    and     edx, 0xf
    lea     r9, [rip + hexdig]
    movzx   edx, byte ptr [r9 + rdx]
    mov     [rsp + rcx], dl
    shr     rax, 4
    test    rcx, rcx
    jz      .eo_prontos
    dec     rcx
    jmp     .eo_nibble
.eo_prontos:
    # 16 dígitos prontos em [rsp..rsp+15]. Acha o primeiro não-zero entre os
    # 8 mais significativos (índices 0..7); se todos forem '0', começa em 8.
    xor     ecx, ecx
.eo_busca:
    cmp     ecx, 8
    jge     .eo_comeco_8
    cmp     byte ptr [rsp + rcx], '0'
    jne     .eo_emite
    inc     ecx
    jmp     .eo_busca
.eo_comeco_8:
    mov     ecx, 8
.eo_emite:
    lea     rsi, [rsp + rcx]
    mov     edx, 16
    sub     edx, ecx
    call    emit_buf
    add     rsp, 24
    ret

# formatar_linha() -> monta e acrescenta a 'saida' uma linha completa, a
# partir de g_offset, g_nlinha e do conteúdo de 'linha'. Garante espaço no
# buffer de saída antes de começar (uma linha nunca passa de ~70 bytes).
#
# Usa r12 (n = bytes válidos na linha), r13 (colunas já emitidas no campo
# hex, depois reaproveitado para o número de espaços de preenchimento) e
# r14 (índice do grupo, 0..7) como estado que precisa sobreviver às
# chamadas de emit_byte/hex2/emit_buf/escrever_offset — nenhuma delas toca
# r12-r14 (ver comentário de cada uma), então não é preciso recarregar
# nada entre uma chamada e a próxima, exceto rax/rdx/rdi/rsi, que são
# recalculados a cada uso porque são destruídos pelas chamadas.
formatar_linha:
    push    r12
    push    r13
    push    r14
    mov     rax, [rip + g_saida_n]
    cmp     rax, 8192 - 128
    jle     .fl_tem_espaco
    call    flush_saida
.fl_tem_espaco:
    mov     rdi, [rip + g_offset]
    call    escrever_offset
    mov     al, ':'
    call    emit_byte

    mov     r12, [rip + g_nlinha]   # n = bytes válidos nesta linha
    xor     r13d, r13d              # colunas já emitidas no campo hex
    xor     r14d, r14d              # grupo atual (0..7)
.fl_grupo:
    cmp     r14, 8
    jge     .fl_grupos_prontos
    mov     rax, r14
    shl     rax, 1                  # i0 = 2*grupo
    cmp     rax, r12
    jge     .fl_grupos_prontos      # i0 >= n: nenhum grupo a mais tem byte
    mov     al, ' '
    call    emit_byte
    inc     r13
    mov     rax, r14
    shl     rax, 1                  # i0 outra vez (emit_byte destruiu rax)
    lea     rdx, [rip + linha]
    movzx   edi, byte ptr [rdx + rax]
    call    hex2
    add     r13, 2
    mov     rax, r14
    shl     rax, 1
    inc     rax                     # i1 = i0 + 1
    cmp     rax, r12
    jge     .fl_prox_grupo
    lea     rdx, [rip + linha]      # recarrega: hex2 destruiu rdx
    movzx   edi, byte ptr [rdx + rax]
    call    hex2
    add     r13, 2
.fl_prox_grupo:
    inc     r14
    jmp     .fl_grupo
.fl_grupos_prontos:
    mov     rax, 40
    sub     rax, r13
    mov     r13, rax                # r13 agora é o nº de espaços de preenchimento
.fl_pad:
    cmp     r13, 0
    jle     .fl_pad_feito
    mov     al, ' '
    call    emit_byte
    dec     r13
    jmp     .fl_pad
.fl_pad_feito:
    mov     al, ' '                 # as duas colunas antes do ASCII
    call    emit_byte
    call    emit_byte

    lea     rsi, [rip + linha]      # rsi/rdx: nenhuma sub-rotina chamada
    mov     rdx, r12                # abaixo os toca, então não precisam
.fl_ascii:                          # ser recarregados a cada iteração
    test    rdx, rdx
    jz      .fl_ascii_feito
    movzx   eax, byte ptr [rsi]
    cmp     al, 0x20
    jb      .fl_ponto
    cmp     al, 0x7e
    ja      .fl_ponto
    jmp     .fl_literal
.fl_ponto:
    mov     al, '.'
.fl_literal:
    call    emit_byte
    inc     rsi
    dec     rdx
    jmp     .fl_ascii
.fl_ascii_feito:
    mov     al, 10                  # '\n'
    call    emit_byte

    pop     r14
    pop     r13
    pop     r12
    ret

    .section .note.GNU-stack,"",@progbits
