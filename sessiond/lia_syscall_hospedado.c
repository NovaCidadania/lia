/* lia-sessiond — ponte para rodar cli/sistema.c e cli/ferramentas.c (que
 * são código do núcleo sem libc, ver docs/abi.md) dentro de um processo
 * hospedado normal (libc + GLib).
 *
 * `lia_syscall` nestes arquivos normalmente vem de nucleo/x86_64/entrada.S
 * (que também define `_start` — não dá para linkar isso aqui, colidiria
 * com o `_start`/`main` normais do daemon). Esta versão faz a mesma coisa
 * usando a `syscall()` da libc, com uma diferença importante de
 * convenção: a `syscall()` da libc retorna -1 e seta `errno` em caso de
 * erro (convenção de C), enquanto o contrato interno da Lia (docs/abi.md
 * §3) espera `-errno` direto no valor de retorno (convenção do kernel) —
 * esta função converte uma convenção na outra.
 *
 * `lia_envp`/`lia_out`/`lia_err`/`lia_getenv` são as mesmas variáveis
 * globais que cli/main.c define para o binário `lia` — redefinidas aqui
 * (em vez de linkar cli/main.c) de propósito: cli/main.c também referencia
 * cmd_shell/cmd_sudo/cmd_root (cli/privilegio.c), que o daemon nunca deve
 * sequer conseguir alcançar no link.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../runtime/lia.h"
#include "../cli/cli.h"

long lia_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long r = syscall(n, a1, a2, a3, a4, a5, a6);
    if (r == -1 && errno != 0)
        return -(long)errno;
    return r;
}

char **lia_envp;
Buf lia_out, lia_err;

const char *lia_getenv(const char *nome)
{
    size_t n = lia_strlen(nome);
    for (char **e = lia_envp; e && *e; e++)
        if (lia_strncmp(*e, nome, n) == 0 && (*e)[n] == '=')
            return *e + n + 1;
    return 0;
}
