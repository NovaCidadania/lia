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

/* sistema.c — sistema.processos_top_cpu: coleta e cálculo expostos em
 * funções puras/testáveis separadamente do subcomando de CLI (que só
 * orquestra: amostra, dorme, amostra de novo, calcula, ordena, imprime).
 * 'raiz' é sempre injetável (nunca hardcoded "/proc" dentro destas duas
 * funções) para permitir testar com uma árvore de fixtures, sem depender
 * da máquina real nem de qualquer timing — ver testes/nucleo/. */
#define LIA_PROC_NOME_MAX 32

typedef struct {
    u32 pid;
    char nome[LIA_PROC_NOME_MAX]; /* "comm" do kernel — nunca cmdline/environ */
    char estado;                  /* letra crua do campo 3 de /proc/<pid>/stat */
    u64 ticks;                    /* utime+stime, em USER_HZ (LIA_CLK_TCK, 100/s) */
} LiaProcAmostra;

typedef struct {
    u32 pid;
    char nome[LIA_PROC_NOME_MAX];
    char estado;
    u64 cpu_centesimos;           /* percentual de CPU * 100 (ex.: 1234 = 12,34%) */
    u64 tempo_cpu_centesimos_s;   /* tempo de CPU acumulado desde o início do processo, em centésimos de segundo */
} LiaProcCpu;

/* Lê <raiz>/<pid>/stat para cada entrada de <raiz> cujo nome seja
 * inteiramente numérico (um PID). Uma entrada cujo stat esteja ausente,
 * truncado, ou com o campo de estado ou utime/stime não numérico é
 * silenciosamente ignorada (processo pode ter desaparecido entre o
 * listar e o ler, ou 'raiz' não é um /proc de verdade) — nunca falha a
 * coleta inteira por isso. Nunca lê /proc/<pid>/cmdline nem
 * /proc/<pid>/environ, nunca segue link simbólico (o nome do PID já vem
 * do próprio listar; abrir "<raiz>/<pid>/stat" não segue nada além do
 * que o kernel resolve para esse caminho em si — não há link em
 * /proc/<pid>/stat). Retorna a quantidade escrita em 'out' (até 'max',
 * excesso é silenciosamente descartado — mesmo limite de memória fixo
 * que o resto deste arquivo já usa para listas), ou -1 se 'raiz' em si
 * não pôde ser aberta como diretório (ex.: /proc não montado). */
long lia_proc_coletar_amostra(const char *raiz, LiaProcAmostra *out, size_t max);

/* Cruza duas amostras (mesmo pid presente nas duas — um pid só numa das
 * duas, seja porque era novo ou porque terminou no intervalo, é
 * ignorado) e calcula o percentual de uso de CPU no intervalo entre
 * elas, normalizado por 'ncpus' (0 nunca é aceito — comportamento
 * indefinido para o chamador verificar antes). Retorna a quantidade
 * escrita em 'out' (até 'max'), já ordenada por cpu_centesimos
 * decrescente (empate: pid crescente, para ordem determinística). Função
 * pura: nunca faz E/S, nunca depende de relógio — 'elapsed_ms' já vem
 * calculado pelo chamador, o que a torna testável com dados sintéticos. */
size_t lia_proc_calcular_cpu(const LiaProcAmostra *antes, size_t n_antes,
                              const LiaProcAmostra *depois, size_t n_depois,
                              u64 elapsed_ms, u64 ncpus,
                              LiaProcCpu *out, size_t max);

#endif
