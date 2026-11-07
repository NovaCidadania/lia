# ABI interna — Assembly ↔ C

Convenção de chamada entre o núcleo em Assembly (`nucleo/`) e o runtime/CLI em C (`runtime/`, `cli/`). Referenciado pelo PLANO.md §2.1, regra 3.

## 1. Convenção de chamada: x86-64 System V

A Lia usa a **ABI padrão do Linux x86-64 (System V AMD64)**, sem desvios — isso permite que Assembly e C chamem um ao outro livremente, incluindo o `gcc` como assembler (`as` embutido via `.intel_syntax noprefix`).

### Passagem de argumentos (chamada de função, não syscall)

| Ordem | Inteiro/ponteiro | Ponto flutuante |
|---|---|---|
| 1º | `rdi` | `xmm0` |
| 2º | `rsi` | `xmm1` |
| 3º | `rdx` | `xmm2` |
| 4º | `rcx` | `xmm3` |
| 5º | `r8` | `xmm4` |
| 6º | `r9` | `xmm5` |
| 7º+ | pilha (da direita para a esquerda), 8 bytes por slot, acima do endereço de retorno | pilha |

- Valor de retorno inteiro/ponteiro: `rax` (e `rdx` para um par 128-bit, ex. divisão/retorno de struct pequena).
- Valor de retorno flutuante: `xmm0`.

### Registradores preservados (*callee-saved*) vs destruídos (*caller-saved*)

| Preservados pela função chamada (deve salvar/restaurar se usar) | Pode destruir livremente (quem chama assume que mudam) |
|---|---|
| `rbx`, `rbp`, `rsp`, `r12`, `r13`, `r14`, `r15` | `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8`–`r11` |

As rotinas do núcleo (`nucleo/x86_64/*.S`) seguem isso mesmo sem usar `call`/`ret` de C em todo lugar: `lia-cat.S`, por exemplo, usa `r12`–`r15` como registradores de estado do programa inteiro porque são preservados por qualquer sub-rotina interna (`copiar`, `escreve_err`) que ela mesma chama.

### Alinhamento de stack

- `rsp` deve estar alinhado a **16 bytes no momento de um `call`** (ou seja: imediatamente *antes* do `call`, `rsp % 16 == 0`; depois do `call` empurrar o endereço de retorno, `rsp % 16 == 8` dentro da função chamada).
- Instruções SSE/AVX com operando de memória alinhado (`movaps`, etc.) dependem disso. O código atual usa principalmente `movups`/escalar e buffers alinhados manualmente (`.align 64` em `lia-cat.S`), mas o alinhamento de pilha é mantido de qualquer forma, para já estar correto quando SIMD alinhado for introduzido (fase de kernels, L0/L3).
- Em `_start` (`nucleo/x86_64/entrada.S`), o kernel entrega `rsp` apontando para `argc` **sem** garantia dos 16 bytes exigidos por uma função C comum. Por isso `_start` faz `and rsp, -16` antes do primeiro `call lia_main` — esse é o único lugar do programa em que o alinhamento é restaurado "do zero"; depois disso, todo `call`/`ret` em cadeia preserva o invariante.
- Nenhuma função do núcleo usa `alloca` ou VLA; o tamanho de frame de cada rotina é estático e, quando há `sub rsp, N`, `N` é múltiplo de 16.

## 2. Syscalls (chamada direta ao kernel, não ABI de função)

Diferente de uma chamada de função C, uma `syscall` no Linux x86-64 usa sua própria convenção, implementada em `lia_syscall` (`nucleo/x86_64/entrada.S`):

| Syscall | Função C (`lia_syscall`) |
|---|---|
| número | `rax` | `rdi` (1º argumento lógico) |
| arg 1 | `rdi` | `rsi` |
| arg 2 | `rsi` | `rdx` |
| arg 3 | `rdx` | `rcx` |
| arg 4 | `r10` (não `rcx` — `syscall` destrói `rcx`) | `r8` |
| arg 5 | `r8` | `r9` |
| arg 6 | `r9` | pilha |
| retorno | `rax` (negativo = `-errno`) | `rax` |

`lia_syscall` existe exatamente para absorver essa diferença: do lado de C/chamador, ela parece uma função comum (ABI padrão, 6 argumentos inteiros); do lado de dentro, ela remapeia para a convenção de `syscall` antes de executar a instrução. `syscall` também destrói `rcx` e `r11` (usados internamente pela instrução para salvar `rip`/`rflags`), por isso eles **não** aparecem como "preservados" na tabela da seção 1 quando atravessam uma syscall — são caller-saved de qualquer forma na ABI padrão, então isso não exige tratamento especial.

## 3. Representação de erros

- Toda função do núcleo e do runtime que envolve uma operação que pode falhar no SO retorna **`long`**, com:
  - `>= 0`: sucesso (valor útil: bytes lidos/escritos, descritor, etc.).
  - `< 0`: falha; o valor é **`-errno`** (ex.: `-2` = `ENOENT`, `-13` = `EACCES`, `-4` = `EINTR`).
- Não há exceções, `errno` global, nem `setjmp`/`longjmp` no núcleo. O chamador sempre decide na hora, a partir do valor de retorno.
- `EINTR` (`-4`) é tratado por retry na própria rotina de I/O (ver `copiar` em `lia-cat.S`, e `lia_wait4`/loops de leitura em C) — nunca propagado para quem chamou como se fosse um erro definitivo.
- Leitura/escrita **parcial** (retorno `> 0` mas menor que o pedido) nunca é tratada como erro: o chamador deve, e o código atual faz, continuar o laço até completar ou até `read` retornar `0` (EOF) ou negativo.
- Funções que não envolvem syscall (parsing, formatação em `runtime/texto.c`) preferem retorno por valor direto (ex.: `size_t`, ponteiro) ou um campo `err` acumulado no `Buf` (ver `runtime/lia.h`), em vez de código de erro por chamada — menos ramos em caminho de formatação de saída, que não interage com o SO.

## 4. Convenções de nomes (`lia_*`)

- Todo símbolo exportado por `nucleo/` ou `runtime/` tem prefixo `lia_` (PLANO.md §6). Isso evita colisão com qualquer libc, mesmo que um objeto externo seja linkado por engano, e deixa óbvio, ao ler um `nm`/backtrace, o que é núcleo da Lia.
- Nomes de syscall mantêm o nome em inglês da chamada POSIX correspondente (`lia_read`, `lia_openat`, `lia_fstatat`) — mais fácil de conferir contra `man 2` e contra implementações de referência (musl, llama.cpp etc.) do que um nome traduzido. Esta é a resposta à questão aberta §8.6 do PLANO.md: **manter em inglês** para primitivas POSIX; nomes de domínio da Lia (ferramentas, conceitos próprios) continuam em português (`cmd_shell`, `lia_preparar`, `ferramentas.lia`).
- Rótulos locais dentro de uma rotina `.S` usam `.` (ex.: `.ler`, `.esc`) — símbolo local ao arquivo, não exportado, sem risco de colisão entre rotinas (GNU `as` os trata como locais quando prefixados com `.L`/`.`).
- `lia_main` é o único ponto de entrada em C esperado por `_start`; toda CLI/binário do núcleo define exatamente um `lia_main(long argc, char **argv, char **envp)`.

## 5. Interface entre Assembly e C

- **Quem fornece `_start`:** `nucleo/x86_64/entrada.S`. Ele extrai `argc`/`argv`/`envp` da pilha como o kernel entrega (sem `crt0` de libc), alinha `rsp` e chama `lia_main`. O valor de retorno de `lia_main` vira o código de saída do processo via `exit_group`.
- **Quem fornece a ponte de syscall:** também `entrada.S`, via `lia_syscall` — toda chamada ao kernel, de Assembly ou de C, passa por aqui (ou por uma instrução `syscall` equivalente dentro de uma rotina ASM dedicada, como em `lia-cat.S`, quando o ganho de não pagar uma chamada de função extra foi medido).
- **Quem fornece os wrappers tipados:** `runtime/sys.c`, uma função por syscall (`lia_read`, `lia_write`, `lia_openat`, `lia_close`, `lia_fork`, `lia_execve`, ...), todas implementadas como `return lia_syscall(numero, ...)`. C não precisa saber números de syscall nem a convenção de registrador da seção 2: só chama a função como qualquer outra.
- **Cabeçalho único:** `runtime/lia.h` declara todos os protótipos `lia_*`, as `struct` que espelham layout de kernel (`struct lia_stat`, `struct lia_dirent`, `struct lia_timespec`, `struct lia_utsname`) e as constantes de flags (`O_RDONLY`, `AT_FDCWD`, etc.). Qualquer arquivo C do projeto inclui só este header para falar com o núcleo — nunca `<unistd.h>`/`<fcntl.h>` da libc (o build é `-ffreestanding -nostdlib`, então eles nem estariam disponíveis de forma consistente).
- **Programas 100% Assembly** (`lia-cat.S`, `lia-echo.S`): também definem seu próprio `_start` e fazem `syscall` diretamente, sem passar por `lia_syscall`/C. Isso é deliberado — são a prova de que o núcleo funciona sem *nenhuma* dependência de C, e servem de comparação de desempenho/tamanho contra a implementação de referência em C (`nucleo/referencia/`, que usa `entrada.S` + `runtime/sys.c` do jeito normal). Ver PLANO.md §2.1, regra 1.
- **Nenhuma variável global mutável compartilhada** entre Assembly e C além dos buffers estáticos já declarados em `.bss`/`.data` com nome próprio (ex.: `buf` em `lia-cat.S`, com `.align 64`); todo estado de uma execução passa por argumento/retorno.

## 6. Primitivas de memória: `lia_memcpy`/`lia_memset`/`lia_memcmp`

Implementadas em `nucleo/x86_64/lia-mem.S` (três backends) e `nucleo/x86_64/lia-cpuid.S`
(detecção de capacidade), despachadas por `runtime/lia_mem.c`. Oráculo escalar em C
para testes: `nucleo/referencia/lia_mem_ref.c`.

### Contrato (igual nos três backends e no despacho)

- `lia_memcpy(dst, src, n)` → retorna `dst`. **Não aceita regiões sobrepostas** —
  comportamento indefinido se `dst`/`src` se sobrepuserem (como o `memcpy` padrão).
  Para sobreposição, usa-se `memmove` de `runtime/texto.c` (implementação em C,
  fora do escopo desta etapa — ver "Decisão: sem `memmove` SIMD" abaixo).
- `lia_memset(dst, c, n)` → retorna `dst`; `c` é truncado para `unsigned char`.
- `lia_memcmp(a, b, n)` → retorna `0` se os `n` bytes forem iguais; senão, a
  diferença (com sinal) entre os primeiros bytes que diferem — só o **sinal** é
  garantido (igual ao `memcmp` de `runtime/texto.c`), não o valor exato.
- **`n == 0` nunca lê nem escreve memória**, para qualquer valor de `dst`/`src`/`a`/`b`
  (inclusive um ponteiro nulo ou inválido) — testado explicitamente em
  `testes/mem/teste_mem.c`.
- **Sem exigência de alinhamento**: todo acesso é via `mov`/`movdqu`/`vmovdqu`
  (carga/armazenamento não alinhado). Alinhar `dst`/`src` pode melhorar desempenho
  em outras implementações, mas não é exigido pela Lia.
- **Retorno/registradores**: seguem a ABI padrão da seção 1 — `rax` como retorno,
  `rbx`/`rbp`/`r12`-`r15` nunca tocados por nenhuma das rotinas (não precisam de
  `push`/`pop`). As variantes AVX2 executam `vzeroupper` antes de retornar (evita
  penalidade de transição SSE/AVX em quem chamar depois usando SSE legado — boa
  prática de performance, não é exigência da ABI).
- **Limites**: nenhuma rotina lê ou escreve um byte além do intervalo `[ptr, ptr+n)`
  pedido — os backends vetoriais processam em blocos (16 bytes SSE2, 32 bytes AVX2)
  e tratam o resto com um laço de bytes, nunca "arredondando para cima" sobre o
  buffer. Ponteiros inválidos (fora de memória mapeada) não são validados pela
  rotina — como em qualquer `memcpy`/`memset`/`memcmp` de baixo nível, é
  responsabilidade de quem chama garantir que o intervalo é válido quando `n > 0`.

### Detecção de CPU (`lia-cpuid.S`) — nunca assume suporte

- `lia_cpuid_sse2()`: checa `CPUID.1:EDX[26]`. SSE2 é parte obrigatória da
  arquitetura x86-64 (todo processador capaz de rodar este binário já o suporta),
  mas o bit é checado de qualquer forma, por auditabilidade.
- `lia_cpuid_avx2()`: só retorna 1 se **todas** as condições a seguir forem
  verdadeiras, na ordem: `CPUID.1:ECX.OSXSAVE[27]` (o SO salva/restaura estado
  estendido) **e** `CPUID.1:ECX.AVX[28]`; em seguida `XGETBV` com `XCR0` bits 1
  (SSE) e 2 (AVX) ligados (confirma que o SO de fato habilitou o estado, não só
  que o anuncia); por fim `CPUID.7.0:EBX.AVX2[5]`. A instrução `XGETBV` só é
  executada depois de confirmado `OSXSAVE` — executá-la sem isso pode gerar
  `#UD` (instrução ilegal) em alguns sistemas.
- Nenhuma rotina AVX2/SSE2 é executada sem essa verificação prévia — nem mesmo
  para medir desempenho "só para ver". A seleção manual de backend
  (`lia_mem_selecionar`, abaixo) reforça isso: forçar um backend que a CPU não
  suporta é rejeitado (retorno `-1`), nunca executado.

### Despacho (`runtime/lia_mem.c`)

- Automático por padrão: `AVX2 > SSE2 > escalar`, conforme `lia_mem_capacidade()`
  (bitmask: bit0 = SSE2, bit1 = AVX2) detectada uma vez por processo.
- `lia_mem_selecionar(modo)` permite forçar um backend (`LIA_MEM_AUTO`,
  `LIA_MEM_ESCALAR`, `LIA_MEM_SSE2`, `LIA_MEM_AVX2`) — pensado para testes e
  benchmark, não para uso normal. Um `modo` fora desse conjunto, ou um backend
  vetorial que a CPU não suporta, é **rejeitado** (retorna `-1`, nada é alterado);
  nunca chega a executar uma instrução que o processador não entende.
- `lia_mem_backend_ativo()` resolve `LIA_MEM_AUTO` para o backend concreto em uso
  (para relatório em testes/benchmark).

### Decisão: sem `memmove` SIMD nesta etapa

`lia_memcpy` (e seus backends) assume explicitamente que `dst`/`src` não se
sobrepõem — é a mesma semântica do `memcpy` de `runtime/texto.c` e do `memcpy`
padrão. Implementar um `lia_memmove` vetorizado corretamente exigiria detectar a
direção da sobreposição e inverter o sentido da cópia por bloco (como já faz o
`memmove` escalar em `runtime/texto.c`), o que muda a lógica de laço dos três
backends. Decisão explícita: **não fazer isso nesta etapa** — `memmove` continua
disponível, sem SIMD, em `runtime/texto.c`, para os casos (raros, no código atual)
que precisam de sobreposição. Se/quando `lia_memmove` vetorizado for necessário,
é um próximo passo isolado, com seu próprio benchmark.

### Build

O caminho comum (`make`, `make teste`) não depende de `-mavx2`/`-msse2`: os
backends são Assembly manual (mnemônicos `vmovdqu`/`vpcmpeqb`/etc. direto no
`.S`), então o binário resultante roda em qualquer CPU x86-64 — a seleção de
qual backend *executar* é feita em runtime por `lia_mem_selecionar`/CPUID, nunca
pelo compilador. `build/lia-mem-teste` e `build/lia-mem-bench` chamam os três
backends diretamente, então também servem de prova de que o escalar funciona
mesmo quando SSE2/AVX2 estão disponíveis mas desligados via seleção manual.

## 8. Plano futuro para AArch64

Ainda não implementado (fase 11 do PLANO.md). Decisões já tomadas para quando chegar a vez:

- **Convenção de chamada:** AAPCS64. Argumentos inteiros/ponteiro em `x0`–`x7` (em vez de `rdi,rsi,rdx,rcx,r8,r9`), retorno em `x0` (par 128-bit em `x0`/`x1`). Preservados: `x19`–`x28`, `sp`, `x29` (frame pointer), `x30` (link register, análogo ao endereço de retorno — mas é um registrador, não a pilha, então uma rotina que chama outra precisa salvar `x30` explicitamente se for usar `bl`/`ret` aninhado).
- **Syscall:** instrução `svc #0`, número da syscall em `x8` (não `rax`), argumentos em `x0`–`x5` **na mesma ordem lógica** (sem o desvio do 4º argumento que existe em x86-64) — então `lia_syscall` em AArch64 é mais direto de implementar que a versão x86-64, só precisa mover `x0..x5` para a posição que a convenção de chamada já deixou, sem reordenar.
- **Alinhamento de stack:** `sp` deve estar alinhado a 16 bytes a todo instante em código de usuário AArch64 (mais estrito que x86-64, que só exige no momento do `call`) — o `_start` de AArch64 precisa garantir isso desde a entrada.
- **Números de syscall e layout de struct:** `struct lia_stat`/`struct lia_dirent`/etc. em `runtime/lia.h` precisam de uma variante por arquitetura (`#ifdef __aarch64__` ou arquivo separado), porque o layout de `stat`/`dirent64` do kernel não é garantido idêntico entre arquiteturas — isso vai virar `runtime/lia_aarch64.h` ou equivalente quando o port começar, nunca um `#ifdef` espalhado pelo código de lógica.
- **O que não muda:** o prefixo `lia_`, a regra de retorno `-errno`, o conceito de implementação de referência em C como oráculo, e a divisão de arquivos por arquitetura em `nucleo/<arch>/` (já é a estrutura atual: `nucleo/x86_64/`; `nucleo/aarch64/` é só criar quando a fase 11 começar).
