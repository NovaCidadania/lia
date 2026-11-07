#ifndef CLI_H
#define CLI_H

#include "../runtime/lia.h"

#define LIA_VERSAO "0.1.0"

extern char **lia_envp;
extern Buf lia_out, lia_err;

#define PROBE_SAIDA 4096

typedef struct {
    int exec_errno, saida, estourou;
    u32 ms;
    char out[PROBE_SAIDA], err[PROBE_SAIDA];
    size_t nout, nerr;
} Res;

const char *lia_getenv(const char *nome);

/* ferramentas.c */
int lia_preparar(void);          /* cria ~/.lia/...; 0 = ok */
const char *lia_home(void);
int lia_juntar(char *dst, size_t cap, const char *a, const char *b, const char *c);
/* fork+execve com argv/env explícitos; captura stdout/stderr separados, timeout, sem shell */
void lia_executar(const char *caminho, const char *const *argv, const char *const *env, Res *r, u32 limite_ms);

/* privilegio.c */
int cmd_shell(int argc, char **argv);
int cmd_sudo(int argc, char **argv);
int cmd_root(int argc, char **argv);

/* sistema.c — somente leitura */
int cmd_system(int argc, char **argv);
int cmd_tools(int argc, char **argv);

#endif
