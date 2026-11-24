/* Execução isolada de processos — ver projeto/processo.h. */
#define _GNU_SOURCE
#include "processo.h"
#include <glib/gstdio.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BWRAP "/usr/bin/bwrap"

void proc_resultado_liberar(ProcResultado *r)
{
    if (!r) return;
    g_free(r->saida); g_free(r->erros); g_free(r->erro);
    memset(r, 0, sizeof *r);
}

static const char *const AMBIENTE_BASE[] = {
    "PATH=/usr/local/bin:/usr/bin", "HOME=/tmp/home", "LANG=C.UTF-8", "LC_ALL=C.UTF-8", "TERM=dumb", "TMPDIR=/tmp",
    "NO_COLOR=1", NULL,
};

char *proc_descricao_ambiente(char **env_extra)
{
    GString *o = g_string_new(NULL);
    for (int i = 0; AMBIENTE_BASE[i]; i++) g_string_append_printf(o, "%s%s", i ? " " : "", AMBIENTE_BASE[i]);
    for (int i = 0; env_extra && env_extra[i]; i++) {
        /* nomes e e-mails de autor do Git aparecem; nada mais é acrescentado */
        g_string_append_printf(o, " %s", env_extra[i]);
    }
    g_string_append(o, " (nada herdado do daemon)");
    return g_string_free(o, FALSE);
}

const char *proc_descricao_isolamento(gboolean projeto_gravavel, gboolean git_gravavel)
{
    if (git_gravavel) return "bubblewrap: sem rede, sem /home, sem /sys, sistema somente leitura, /tmp vazio, só o projeto montado (incluindo .git/, gravável para o commit)";
    if (projeto_gravavel) return "bubblewrap: sem rede, sem /home, sem /sys, sistema somente leitura, /tmp vazio, só o projeto montado e gravável, .git/ somente leitura";
    return "bubblewrap: sem rede, sem /home, sem /sys, sistema somente leitura, /tmp vazio, projeto montado somente leitura";
}

static void bw(GPtrArray *a, const char *s) { g_ptr_array_add(a, g_strdup(s)); }

char **proc_argv_isolado(const ProcPedido *p)
{
    GPtrArray *a = g_ptr_array_new();
    bw(a, BWRAP);
    bw(a, "--unshare-all");        /* usuário, PID, rede, IPC, UTS, cgroup */
    bw(a, "--die-with-parent");
    bw(a, "--new-session");        /* sem acesso ao terminal de controle */
    bw(a, "--clearenv");
    bw(a, "--ro-bind"); bw(a, "/usr"); bw(a, "/usr");
    bw(a, "--symlink"); bw(a, "usr/bin"); bw(a, "/bin");
    bw(a, "--symlink"); bw(a, "usr/bin"); bw(a, "/sbin");
    bw(a, "--symlink"); bw(a, "usr/lib"); bw(a, "/lib");
    bw(a, "--symlink"); bw(a, "usr/lib"); bw(a, "/lib64");
    bw(a, "--ro-bind"); bw(a, "/etc"); bw(a, "/etc");
    bw(a, "--dev"); bw(a, "/dev");
    bw(a, "--proc"); bw(a, "/proc");
    bw(a, "--tmpfs"); bw(a, "/tmp");
    bw(a, "--dir"); bw(a, "/tmp/home");
    bw(a, "--dir"); bw(a, "/tmp/sem-ganchos"); /* core.hooksPath vazio para o Git */
    bw(a, p->projeto_gravavel || p->git_gravavel ? "--bind" : "--ro-bind"); bw(a, p->raiz); bw(a, p->raiz);
    char *git = g_build_filename(p->raiz, ".git", NULL);
    if (g_file_test(git, G_FILE_TEST_IS_DIR)) {
        bw(a, p->git_gravavel ? "--bind" : "--ro-bind"); bw(a, git); bw(a, git);
    }
    g_free(git);
    bw(a, "--chdir"); bw(a, p->raiz);
    for (int i = 0; AMBIENTE_BASE[i]; i++) {
        const char *eq = strchr(AMBIENTE_BASE[i], '=');
        bw(a, "--setenv");
        g_ptr_array_add(a, g_strndup(AMBIENTE_BASE[i], (gsize)(eq - AMBIENTE_BASE[i])));
        bw(a, eq + 1);
    }
    for (int i = 0; p->env_extra && p->env_extra[i]; i++) {
        const char *eq = strchr(p->env_extra[i], '=');
        if (!eq) continue;
        bw(a, "--setenv");
        g_ptr_array_add(a, g_strndup(p->env_extra[i], (gsize)(eq - p->env_extra[i])));
        bw(a, eq + 1);
    }
    bw(a, "--");
    for (int i = 0; p->argv && p->argv[i]; i++) bw(a, p->argv[i]);
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

typedef struct { rlim_t memoria, cpu; } Limites;

static void antes_do_exec(gpointer dados)
{
    /* roda no filho, entre fork e exec: só chamadas async-signal-safe */
    const Limites *l = dados;
    struct rlimit r;
    r.rlim_cur = r.rlim_max = l->memoria; setrlimit(RLIMIT_AS, &r);
    r.rlim_cur = r.rlim_max = l->cpu; setrlimit(RLIMIT_CPU, &r);
    r.rlim_cur = r.rlim_max = (rlim_t)512 * 1024 * 1024; setrlimit(RLIMIT_FSIZE, &r);
    r.rlim_cur = r.rlim_max = 0; setrlimit(RLIMIT_CORE, &r);
    r.rlim_cur = r.rlim_max = 1024; setrlimit(RLIMIT_NOFILE, &r);
    sigset_t vazio;
    sigemptyset(&vazio);
    sigprocmask(SIG_SETMASK, &vazio, NULL);
}

static gboolean g_isolamento_testado, g_isolamento_ok;
static char *g_isolamento_detalhe;
G_LOCK_DEFINE_STATIC(isolamento);

gboolean proc_isolamento_disponivel(char **detalhe)
{
    G_LOCK(isolamento);
    if (!g_isolamento_testado) {
        g_isolamento_testado = TRUE;
        if (!g_file_test(BWRAP, G_FILE_TEST_IS_EXECUTABLE)) {
            g_isolamento_detalhe = g_strdup("bubblewrap (" BWRAP ") não está instalado — instale o pacote 'bubblewrap' para permitir a execução de builds e testes");
        } else {
            /* teste real: cria o isolamento sem rede e confere que /home não existe lá dentro */
            char *dir = g_dir_make_tmp("lia-isolamento-XXXXXX", NULL);
            char *sonda[] = { (char *)"/usr/bin/test", (char *)"!", (char *)"-e", (char *)"/home", NULL };
            ProcPedido p = { .argv = sonda, .raiz = dir, .timeout_ms = 10000, .max_saida = 4096, .memoria_mb = 256 };
            char **argv = proc_argv_isolado(&p);
            gint status = -1;
            char *err = NULL;
            GError *ge = NULL;
            gboolean rodou = dir && g_spawn_sync("/", argv, (char **)AMBIENTE_BASE, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, &err, &status, &ge);
            g_strfreev(argv);
            if (dir) { g_rmdir(dir); g_free(dir); }
            if (rodou && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                g_isolamento_ok = TRUE;
                g_isolamento_detalhe = g_strdup("bubblewrap funcional: namespaces de usuário/PID/rede criados, /home invisível");
            } else {
                g_isolamento_detalhe = g_strdup_printf("bubblewrap não conseguiu criar o isolamento (%s) — confira se namespaces de usuário sem privilégio estão habilitados (sysctl kernel.unprivileged_userns_clone=1)",
                                                       ge ? ge->message : (err && *err ? g_strstrip(err) : "falhou"));
            }
            if (ge) g_error_free(ge);
            g_free(err);
        }
    }
    gboolean ok = g_isolamento_ok;
    if (detalhe) *detalhe = g_strdup(g_isolamento_detalhe);
    G_UNLOCK(isolamento);
    return ok;
}

char *proc_resolver_executavel(const char *nome, const char *raiz, char **erro)
{
    if (!nome || !*nome) { *erro = g_strdup("comando vazio"); return NULL; }
    char *c = NULL;
    if (strchr(nome, '/')) {
        if (g_path_is_absolute(nome)) {
            if (!g_str_has_prefix(nome, "/usr/")) { *erro = g_strdup_printf("executável fora de /usr e do projeto: %s", nome); return NULL; }
            c = g_canonicalize_filename(nome, NULL);
        } else {
            c = g_canonicalize_filename(nome, raiz);
            gsize n = strlen(raiz);
            if (strncmp(c, raiz, n) || c[n] != '/') { *erro = g_strdup_printf("executável fora do projeto: %s", nome); g_free(c); return NULL; }
            char *rel = c + n + 1;
            if (g_str_has_prefix(rel, ".git/")) { *erro = g_strdup("executável dentro de .git/ recusado"); g_free(c); return NULL; }
        }
    } else {
        static const char *const DIRS[] = { "/usr/local/bin", "/usr/bin", NULL };
        for (int i = 0; DIRS[i] && !c; i++) {
            char *t = g_build_filename(DIRS[i], nome, NULL);
            if (g_file_test(t, G_FILE_TEST_IS_EXECUTABLE) && !g_file_test(t, G_FILE_TEST_IS_DIR)) c = t; else g_free(t);
        }
        if (!c) { *erro = g_strdup_printf("comando não encontrado em /usr/local/bin nem /usr/bin: %s", nome); return NULL; }
    }
    if (!g_file_test(c, G_FILE_TEST_IS_EXECUTABLE) || g_file_test(c, G_FILE_TEST_IS_DIR)) {
        *erro = g_strdup_printf("não é um executável: %s", c);
        g_free(c);
        return NULL;
    }
    return c;
}

static char *utf8_seguro(GByteArray *b)
{
    char *s = b->len ? g_utf8_make_valid((const char *)b->data, (gssize)b->len) : g_strdup("");
    return s;
}

static void ler_fd(int fd, GByteArray *buf, gsize max, gboolean *truncado, gboolean *fechou)
{
    guint8 tmp[16384];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n > 0) {
            gsize cabe = buf->len < max ? MIN((gsize)n, max - buf->len) : 0;
            if (cabe) g_byte_array_append(buf, tmp, (guint)cabe);
            if ((gsize)n > cabe) *truncado = TRUE; /* continua lendo e descartando: o filho nunca trava no pipe */
            continue;
        }
        if (n == 0) { *fechou = TRUE; return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        *fechou = TRUE;
        return;
    }
}

void proc_executar(const ProcPedido *p, ProcResultado *r)
{
    memset(r, 0, sizeof *r);
    char *det = NULL;
    if (!proc_isolamento_disponivel(&det)) {
        r->erro = g_strdup_printf("execução recusada: isolamento indisponível — %s", det);
        g_free(det);
        return;
    }
    g_free(det);
    if (!p->argv || !p->argv[0] || !g_path_is_absolute(p->argv[0])) { r->erro = g_strdup("executável não resolvido"); return; }
    int timeout = p->timeout_ms > 0 && p->timeout_ms <= PROC_TIMEOUT_MAX_MS ? p->timeout_ms : 120000;
    gsize max = p->max_saida ? p->max_saida : PROC_SAIDA_PADRAO;
    Limites lim = { .memoria = (rlim_t)(p->memoria_mb ? p->memoria_mb : PROC_MEMORIA_PADRAO_MB) * 1024 * 1024,
                    .cpu = (rlim_t)(timeout / 1000 + 5) };
    char **argv = proc_argv_isolado(p);
    GPid pid = 0;
    int fd_out = -1, fd_err = -1;
    GError *ge = NULL;
    gint64 inicio = g_get_monotonic_time();
    gboolean ok = g_spawn_async_with_pipes("/", argv, (char **)AMBIENTE_BASE,
                                           G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CLOEXEC_PIPES | G_SPAWN_STDIN_FROM_DEV_NULL,
                                           antes_do_exec, &lim, &pid, NULL, &fd_out, &fd_err, &ge);
    g_strfreev(argv);
    if (!ok) {
        r->erro = g_strdup_printf("não foi possível iniciar o processo: %s", ge ? ge->message : "?");
        if (ge) g_error_free(ge);
        return;
    }
    r->executou = TRUE;
    fcntl(fd_out, F_SETFL, fcntl(fd_out, F_GETFL) | O_NONBLOCK);
    fcntl(fd_err, F_SETFL, fcntl(fd_err, F_GETFL) | O_NONBLOCK);
    GByteArray *bo = g_byte_array_new(), *be = g_byte_array_new();
    gboolean fim_out = FALSE, fim_err = FALSE, terminou = FALSE;
    int status = 0;
    gint64 morte_em = 0; /* quando mandar SIGKILL depois do SIGTERM */
    while (!terminou || !fim_out || !fim_err) {
        struct pollfd fds[2];
        int n = 0;
        if (!fim_out) { fds[n].fd = fd_out; fds[n].events = POLLIN; n++; }
        if (!fim_err) { fds[n].fd = fd_err; fds[n].events = POLLIN; n++; }
        if (n) poll(fds, (nfds_t)n, 100); else g_usleep(20000);
        if (!fim_out) ler_fd(fd_out, bo, max, &r->stdout_truncado, &fim_out);
        if (!fim_err) ler_fd(fd_err, be, max, &r->stderr_truncado, &fim_err);
        if (!terminou) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) terminou = TRUE;
        }
        gint64 agora = g_get_monotonic_time();
        if (!terminou && !morte_em) {
            gboolean cancelar = p->cancelado && g_atomic_int_get(p->cancelado);
            if (cancelar) r->cancelado = TRUE;
            if ((agora - inicio) / 1000 > timeout) r->timeout = TRUE;
            if (r->cancelado || r->timeout) {
                /* matar o bwrap (init do namespace de PID) encerra a árvore inteira */
                kill(pid, SIGTERM);
                morte_em = agora + 2 * G_USEC_PER_SEC;
            }
        }
        if (!terminou && morte_em && agora >= morte_em) { kill(pid, SIGKILL); morte_em = G_MAXINT64; }
        /* processo acabou mas um neto ficou com o pipe aberto: não espera para sempre */
        if (terminou && (!fim_out || !fim_err) && (agora - inicio) / 1000 > timeout + 3000) { fim_out = fim_err = TRUE; }
    }
    close(fd_out);
    close(fd_err);
    g_spawn_close_pid(pid);
    r->tempo_ms = (g_get_monotonic_time() - inicio) / 1000;
    if (WIFEXITED(status)) { r->codigo_saida = WEXITSTATUS(status); r->sinal = 0; }
    else if (WIFSIGNALED(status)) { r->codigo_saida = -1; r->sinal = WTERMSIG(status); }
    r->saida = utf8_seguro(bo);
    r->erros = utf8_seguro(be);
    g_byte_array_unref(bo);
    g_byte_array_unref(be);
}
