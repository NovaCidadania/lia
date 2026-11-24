/* Execução de processos do projeto em isolamento (bubblewrap).
 *
 * argv explícito evita um shell intermediário — mas NÃO impede que a
 * ferramenta chamada execute comandos: make, scripts de teste e
 * compiladores com plugins rodam código do próprio projeto. Por isso todo
 * processo roda dentro do bwrap, sem exceção:
 *   - namespaces novos (usuário, PID, rede, IPC, UTS, cgroup): sem rede,
 *     sem ver outros processos; matar o bwrap mata a árvore inteira;
 *   - sistema somente leitura (/usr, /etc), sem /home, sem /sys, /tmp vazio;
 *   - só a pasta do projeto montada, gravável ou não conforme o pedido, e
 *     .git/ sempre somente leitura (exceto quando o pedido é um commit);
 *   - ambiente limpo (nada herdado do daemon: nenhum token ou segredo);
 *   - limites de memória, CPU, tamanho de arquivo, tempo e saída.
 * Sem bwrap funcional, nada é executado (falha fechada) — ver
 * proc_isolamento_disponivel. */
#ifndef LIA_PROJETO_PROCESSO_H
#define LIA_PROJETO_PROCESSO_H

#include <glib.h>

#define PROC_TIMEOUT_MAX_MS (30 * 60 * 1000)
#define PROC_SAIDA_PADRAO (256 * 1024)
#define PROC_MEMORIA_PADRAO_MB 2048

typedef struct {
    char **argv;              /* argv[0]: caminho absoluto já resolvido (proc_resolver_executavel) */
    const char *raiz;         /* pasta do projeto (montada no mesmo caminho) */
    gboolean projeto_gravavel;
    gboolean git_gravavel;    /* só para git.commit */
    char **env_extra;         /* "CHAVE=valor" acrescentados ao ambiente limpo */
    int timeout_ms;
    gsize max_saida;          /* por fluxo (stdout e stderr) */
    guint memoria_mb;
    volatile gint *cancelado; /* lido periodicamente; != 0 encerra */
} ProcPedido;

typedef struct {
    gboolean executou;        /* FALSE: nem chegou a rodar (ver erro) */
    int codigo_saida;         /* -1 se terminou por sinal */
    int sinal;
    gboolean timeout, cancelado, stdout_truncado, stderr_truncado;
    char *saida, *erros;      /* UTF-8 válido (bytes inválidos trocados) */
    gint64 tempo_ms;
    char *erro;
} ProcResultado;

void proc_resultado_liberar(ProcResultado *r);

/* bwrap existe e consegue criar o isolamento (testado de verdade, uma vez;
 * o resultado fica guardado). *detalhe: o que foi conferido ou por que não. */
gboolean proc_isolamento_disponivel(char **detalhe);
/* descrição legível do isolamento aplicado (para a prévia de aprovação) */
const char *proc_descricao_isolamento(gboolean projeto_gravavel, gboolean git_gravavel);
/* ambiente que o processo recebe (para a prévia) */
char *proc_descricao_ambiente(char **env_extra);

/* Caminho absoluto do executável: nome simples procurado em /usr/local/bin
 * e /usr/bin; "./x" ou "dir/x" relativo à raiz do projeto (precisa ficar
 * dentro dela). NULL + erro se não existir ou não for executável. */
char *proc_resolver_executavel(const char *nome, const char *raiz, char **erro);

/* argv completo do bwrap + comando (exposto para testes e auditoria) */
char **proc_argv_isolado(const ProcPedido *p);

/* Executa e espera (bloqueia: chame numa thread própria). */
void proc_executar(const ProcPedido *p, ProcResultado *r);

#endif
