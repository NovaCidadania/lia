/* Git do projeto — ver projeto/git.h. */
#include "git.h"
#include "arquivos.h"
#include "processo.h"

#include <string.h>

#define GIT "/usr/bin/git"
#define GIT_SAIDA_MAX (512 * 1024)
/* Memória do git isolado: limite de espaço de endereçamento (RLIMIT_AS).
 * Repositórios grandes não precisam de mais memória — o git é configurado
 * para mapear os packs em janelas pequenas (core.packedGitWindowSize/
 * packedGitLimit), então o uso fica limitado qualquer que seja o tamanho do
 * pack. LIA_SESSIOND_GIT_MEMORIA_MB ajusta, dentro de [128, 4096]. */
#define GIT_MEMORIA_PADRAO_MB 1024
#define GIT_MEMORIA_MIN_MB 128
#define GIT_MEMORIA_MAX_MB 4096

static char *g_autor_nome, *g_autor_email;

void git_definir_autor(const char *autor)
{
    g_clear_pointer(&g_autor_nome, g_free);
    g_clear_pointer(&g_autor_email, g_free);
    if (!autor || !*autor) return;
    const char *lt = strchr(autor, '<'), *gt = lt ? strchr(lt, '>') : NULL;
    if (!lt || !gt || lt == autor) return;
    g_autor_nome = g_strstrip(g_strndup(autor, (gsize)(lt - autor)));
    g_autor_email = g_strndup(lt + 1, (gsize)(gt - lt - 1));
    for (char *q = g_autor_nome; *q; q++) if ((unsigned char)*q < 0x20) *q = ' ';
    for (char *q = g_autor_email; *q; q++) if ((unsigned char)*q < 0x20) *q = ' ';
}

static gboolean repositorio(char **erro)
{
    if (!proj_raiz()) { *erro = g_strdup("nenhum projeto definido (LIA_SESSIOND_PROJETO)"); return FALSE; }
    char *g = g_build_filename(proj_raiz(), ".git", NULL);
    gboolean ok = g_file_test(g, G_FILE_TEST_IS_DIR);
    g_free(g);
    if (!ok) { *erro = g_strdup_printf("%s não é a raiz de um repositório Git (falta .git/)", proj_raiz()); return FALSE; }
    if (!g_file_test(GIT, G_FILE_TEST_IS_EXECUTABLE)) { *erro = g_strdup("git não está instalado em " GIT); return FALSE; }
    return TRUE;
}

guint git_memoria_mb(void)
{
    const char *v = g_getenv("LIA_SESSIOND_GIT_MEMORIA_MB");
    gint64 n = v && *v ? g_ascii_strtoll(v, NULL, 10) : GIT_MEMORIA_PADRAO_MB;
    if (n < GIT_MEMORIA_MIN_MB) n = GIT_MEMORIA_MIN_MB;
    if (n > GIT_MEMORIA_MAX_MB) n = GIT_MEMORIA_MAX_MB;
    return (guint)n;
}

/* git com a configuração controlada; 'args' terminado em NULL */
static gboolean rodar(const char *const *args, gboolean gravavel, char **env_extra, volatile gint *cancelado, ProcResultado *r)
{
    GPtrArray *a = g_ptr_array_new();
    char *safe = g_strdup_printf("safe.directory=%s", proj_raiz());
    const char *base[] = { GIT, "--no-pager", "-c", "core.hooksPath=/tmp/sem-ganchos", "-c", "core.fsmonitor=false",
                           "-c", "core.pager=cat", "-c", "diff.external=", "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
                           "-c", "gc.auto=0", "-c", "maintenance.auto=false", "-c", "color.ui=false", "-c", "core.untrackedCache=false",
                           "-c", "core.editor=false", "-c", "credential.helper=",
                           /* janelas de mmap pequenas: um pack de centenas de MiB não
                            * é mapeado de uma vez (o padrão de 64 bits é 1 GiB por janela) */
                           "-c", "core.packedGitWindowSize=32m", "-c", "core.packedGitLimit=192m",
                           "-c", "core.deltaBaseCacheLimit=48m", "-c", "core.bigFileThreshold=32m",
                           /* sem threads extras (cada uma reserva pilha e arena de memória) */
                           "-c", "core.preloadIndex=false", "-c", "index.threads=1", "-c", "pack.threads=1", "-c", "grep.threads=1",
                           "-c", safe, NULL };
    for (int i = 0; base[i]; i++) g_ptr_array_add(a, (gpointer)base[i]);
    for (int i = 0; args[i]; i++) g_ptr_array_add(a, (gpointer)args[i]);
    g_ptr_array_add(a, NULL);
    GPtrArray *env = g_ptr_array_new();
    const char *fixos[] = { "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null", "GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=false",
                            "GIT_PAGER=cat", "GIT_ASKPASS=", "GIT_OPTIONAL_LOCKS=0", NULL };
    for (int i = 0; fixos[i]; i++) if (gravavel ? strcmp(fixos[i], "GIT_OPTIONAL_LOCKS=0") : TRUE) g_ptr_array_add(env, (gpointer)fixos[i]);
    for (int i = 0; env_extra && env_extra[i]; i++) g_ptr_array_add(env, env_extra[i]);
    g_ptr_array_add(env, NULL);
    ProcPedido p = { .argv = (char **)a->pdata, .raiz = proj_raiz(), .projeto_gravavel = gravavel, .git_gravavel = gravavel,
                     .env_extra = (char **)env->pdata, .timeout_ms = 60000, .max_saida = GIT_SAIDA_MAX, .memoria_mb = git_memoria_mb(),
                     .cancelado = cancelado };
    proc_executar(&p, r);
    g_ptr_array_free(a, TRUE);
    g_ptr_array_free(env, TRUE);
    g_free(safe);
    return r->executou && r->codigo_saida == 0 && !r->timeout && !r->cancelado;
}

static char *falha(ProcResultado *r, const char *o_que)
{
    char *e = !r->executou ? g_strdup_printf("%s: %s", o_que, r->erro ? r->erro : "não executou")
            : r->cancelado ? g_strdup_printf("%s: cancelado", o_que)
            : r->timeout ? g_strdup_printf("%s: tempo esgotado", o_que)
            : g_strdup_printf("%s falhou (saída %d): %s", o_que, r->codigo_saida, r->erros && *r->erros ? g_strstrip(r->erros) : "sem mensagem");
    return e;
}

static char *json_de(JsonBuilder *b)
{
    JsonGenerator *g = json_generator_new();
    JsonNode *r = json_builder_get_root(b);
    json_generator_set_root(g, r);
    char *s = json_generator_to_data(g, NULL);
    json_node_unref(r);
    g_object_unref(g);
    g_object_unref(b);
    return s;
}

static void b_str(JsonBuilder *b, const char *k, const char *v)
{
    json_builder_set_member_name(b, k);
    if (v) json_builder_add_string_value(b, v); else json_builder_add_null_value(b);
}

/* entradas de "status --porcelain=v1": (xy, caminho) */
static GPtrArray *status_entradas(const char *saida, char **ramo)
{
    GPtrArray *e = g_ptr_array_new_with_free_func(g_free);
    char **linhas = g_strsplit(saida ? saida : "", "\n", -1);
    for (int i = 0; linhas[i]; i++) {
        const char *l = linhas[i];
        if (g_str_has_prefix(l, "## ")) { if (ramo) *ramo = g_strdup(l + 3); continue; }
        if (strlen(l) < 4) continue;
        g_ptr_array_add(e, g_strdup(l));
    }
    g_strfreev(linhas);
    return e;
}

/* caminho de uma linha de status ("XY a -> b" vira b; aspas do git tiradas) */
static char *caminho_status(const char *linha)
{
    const char *c = linha + 3;
    const char *seta = strstr(c, " -> ");
    if (seta) c = seta + 4;
    if (*c == '"') {
        char *sem = g_strndup(c + 1, strlen(c) >= 2 ? strlen(c) - 2 : 0);
        char *u = g_strcompress(sem);
        g_free(sem);
        return u;
    }
    return g_strdup(c);
}

char *git_status(volatile gint *cancelado, char **erro)
{
    if (!repositorio(erro)) return NULL;
    ProcResultado r;
    const char *args[] = { "status", "--porcelain=v1", "--branch", "--untracked-files=all", NULL };
    if (!rodar(args, FALSE, NULL, cancelado, &r)) { *erro = falha(&r, "git status"); proc_resultado_liberar(&r); return NULL; }
    char *ramo = NULL;
    GPtrArray *e = status_entradas(r.saida, &ramo);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    b_str(b, "ramo", ramo);
    json_builder_set_member_name(b, "limpo"); json_builder_add_boolean_value(b, e->len == 0);
    json_builder_set_member_name(b, "arquivos");
    json_builder_begin_array(b);
    for (guint i = 0; i < e->len; i++) {
        const char *l = g_ptr_array_index(e, i);
        char xy[3] = { l[0], l[1], 0 };
        char *c = caminho_status(l);
        json_builder_begin_object(b);
        b_str(b, "estado", xy);
        b_str(b, "caminho", c);
        json_builder_end_object(b);
        g_free(c);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    g_free(ramo);
    g_ptr_array_unref(e);
    proc_resultado_liberar(&r);
    return json_de(b);
}

static const char *p_str(JsonObject *o, const char *k)
{
    return o && json_object_has_member(o, k) && JSON_NODE_TYPE(json_object_get_member(o, k)) == JSON_NODE_VALUE &&
           json_node_get_value_type(json_object_get_member(o, k)) == G_TYPE_STRING ? json_object_get_string_member(o, k) : NULL;
}

char *git_diff(JsonObject *params, volatile gint *cancelado, char **erro)
{
    if (!repositorio(erro)) return NULL;
    const char *caminho = p_str(params, "caminho");
    gboolean preparado = params && json_object_has_member(params, "preparado") && json_object_get_boolean_member(params, "preparado");
    char *rel = NULL;
    if (caminho) {
        char *abs = proj_resolver(caminho, FALSE, FALSE, erro);
        if (!abs) return NULL;
        rel = proj_relativo(abs);
        g_free(abs);
    }
    const char *args[10];
    int n = 0;
    args[n++] = "diff"; args[n++] = "--no-ext-diff"; args[n++] = "--no-textconv"; args[n++] = "--no-color";
    if (preparado) args[n++] = "--cached";
    if (rel) { args[n++] = "--"; args[n++] = rel; }
    args[n] = NULL;
    ProcResultado r;
    if (!rodar(args, FALSE, NULL, cancelado, &r)) { *erro = falha(&r, "git diff"); proc_resultado_liberar(&r); g_free(rel); return NULL; }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    b_str(b, "caminho", rel);
    json_builder_set_member_name(b, "preparado"); json_builder_add_boolean_value(b, preparado);
    b_str(b, "diff", r.saida);
    json_builder_set_member_name(b, "truncado"); json_builder_add_boolean_value(b, r.stdout_truncado);
    json_builder_end_object(b);
    proc_resultado_liberar(&r);
    g_free(rel);
    return json_de(b);
}

char *git_log(JsonObject *params, volatile gint *cancelado, char **erro)
{
    if (!repositorio(erro)) return NULL;
    gint64 lim = params && json_object_has_member(params, "limite") ? json_object_get_int_member(params, "limite") : 10;
    if (lim < 1) lim = 1;
    if (lim > 100) lim = 100;
    char *n = g_strdup_printf("-n%" G_GINT64_FORMAT, lim);
    const char *args[] = { "log", n, "--no-color", "--date=iso-strict", "--format=%H%x1f%an%x1f%ad%x1f%s", NULL };
    ProcResultado r;
    gboolean ok = rodar(args, FALSE, NULL, cancelado, &r);
    g_free(n);
    if (!ok) {
        /* repositório sem nenhum commit ainda */
        if (r.executou && r.erros && strstr(r.erros, "does not have any commits")) {
            proc_resultado_liberar(&r);
            return g_strdup("{\"commits\":[]}");
        }
        *erro = falha(&r, "git log");
        proc_resultado_liberar(&r);
        return NULL;
    }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "commits");
    json_builder_begin_array(b);
    char **linhas = g_strsplit(r.saida ? r.saida : "", "\n", -1);
    for (int i = 0; linhas[i]; i++) {
        char **c = g_strsplit(linhas[i], "\x1f", 4);
        if (g_strv_length(c) == 4) {
            json_builder_begin_object(b);
            b_str(b, "commit", c[0]); b_str(b, "autor", c[1]); b_str(b, "data", c[2]); b_str(b, "assunto", c[3]);
            json_builder_end_object(b);
        }
        g_strfreev(c);
    }
    g_strfreev(linhas);
    json_builder_end_array(b);
    json_builder_end_object(b);
    proc_resultado_liberar(&r);
    return json_de(b);
}

/* ------------------------------------------------------------- commit */

typedef struct {
    GPtrArray *arquivos;  /* relativos */
    GPtrArray *estados;   /* "XY" de cada um */
    char *mensagem;
    char *ramo;
    char *head;
} PlanoCommit;

static void plano_liberar(PlanoCommit *p)
{
    if (!p) return;
    if (p->arquivos) g_ptr_array_unref(p->arquivos);
    if (p->estados) g_ptr_array_unref(p->estados);
    g_free(p->mensagem); g_free(p->ramo); g_free(p->head);
    g_free(p);
}

static PlanoCommit *planejar_commit(JsonObject *params, char **erro)
{
    if (!repositorio(erro)) return NULL;
    const char *msg = p_str(params, "mensagem");
    char *m = msg ? g_strstrip(g_strdup(msg)) : NULL;
    if (!m || !*m) { *erro = g_strdup("informe a mensagem do commit"); g_free(m); return NULL; }
    if (strlen(m) > 5000 || !g_utf8_validate(m, -1, NULL)) { *erro = g_strdup("mensagem de commit grande demais ou inválida"); g_free(m); return NULL; }
    for (const char *q = m; *q; q++)
        if ((unsigned char)*q < 0x20 && *q != '\n' && *q != '\t') { *erro = g_strdup("mensagem de commit com caractere de controle"); g_free(m); return NULL; }
    char *gitdir = g_build_filename(proj_raiz(), ".git", NULL);
    static const char *const EM_ANDAMENTO[] = { "MERGE_HEAD", "rebase-merge", "rebase-apply", "CHERRY_PICK_HEAD", "REVERT_HEAD", "BISECT_LOG", NULL };
    for (int i = 0; EM_ANDAMENTO[i]; i++) {
        char *f = g_build_filename(gitdir, EM_ANDAMENTO[i], NULL);
        gboolean tem = g_file_test(f, G_FILE_TEST_EXISTS);
        g_free(f);
        if (tem) { *erro = g_strdup_printf("há uma operação do Git em andamento (%s) — termine-a antes; não faço commit no meio dela", EM_ANDAMENTO[i]); g_free(gitdir); g_free(m); return NULL; }
    }
    g_free(gitdir);
    ProcResultado r;
    const char *st_args[] = { "status", "--porcelain=v1", "--branch", "--untracked-files=all", NULL };
    if (!rodar(st_args, FALSE, NULL, NULL, &r)) { *erro = falha(&r, "git status"); proc_resultado_liberar(&r); g_free(m); return NULL; }
    PlanoCommit *p = g_new0(PlanoCommit, 1);
    p->mensagem = m;
    GPtrArray *e = status_entradas(r.saida, &p->ramo);
    proc_resultado_liberar(&r);
    p->arquivos = g_ptr_array_new_with_free_func(g_free);
    p->estados = g_ptr_array_new_with_free_func(g_free);
    JsonArray *pedidos = params && json_object_has_member(params, "arquivos") && JSON_NODE_TYPE(json_object_get_member(params, "arquivos")) == JSON_NODE_ARRAY
                             ? json_object_get_array_member(params, "arquivos") : NULL;
    if (pedidos && json_array_get_length(pedidos) > 0) {
        for (guint i = 0; i < json_array_get_length(pedidos); i++) {
            const char *c = json_array_get_string_element(pedidos, i);
            char *abs = proj_resolver(c, TRUE, FALSE, erro);
            if (!abs) { g_ptr_array_unref(e); plano_liberar(p); return NULL; }
            char *rel = proj_relativo(abs);
            g_free(abs);
            const char *estado = NULL;
            for (guint k = 0; k < e->len; k++) {
                char *cs = caminho_status(g_ptr_array_index(e, k));
                if (!strcmp(cs, rel)) estado = g_ptr_array_index(e, k);
                g_free(cs);
            }
            if (!estado) { *erro = g_strdup_printf("'%s' não tem alterações para o commit", rel); g_free(rel); g_ptr_array_unref(e); plano_liberar(p); return NULL; }
            g_ptr_array_add(p->arquivos, rel);
            g_ptr_array_add(p->estados, g_strndup(estado, 2));
        }
    } else {
        /* sem lista: TODOS os alterados — mostrados um a um na prévia */
        for (guint k = 0; k < e->len; k++) {
            g_ptr_array_add(p->arquivos, caminho_status(g_ptr_array_index(e, k)));
            g_ptr_array_add(p->estados, g_strndup(g_ptr_array_index(e, k), 2));
        }
    }
    g_ptr_array_unref(e);
    if (p->arquivos->len == 0) { *erro = g_strdup("não há alterações para o commit (árvore limpa)"); plano_liberar(p); return NULL; }
    const char *head_args[] = { "rev-parse", "--verify", "-q", "HEAD", NULL };
    if (rodar(head_args, FALSE, NULL, NULL, &r)) p->head = g_strstrip(g_strdup(r.saida));
    else p->head = g_strdup("(nenhum commit ainda)");
    proc_resultado_liberar(&r);
    return p;
}

char *git_commit_previa(JsonObject *params, char **erro)
{
    PlanoCommit *p = planejar_commit(params, erro);
    if (!p) return NULL;
    GString *o = g_string_new(NULL);
    g_string_append_printf(o, "commit no ramo %s (HEAD %.12s)\n", p->ramo ? p->ramo : "?", p->head);
    g_string_append(o, "mensagem:\n");
    char **ml = g_strsplit(p->mensagem, "\n", -1);
    for (int i = 0; ml[i]; i++) g_string_append_printf(o, "  │ %s\n", ml[i]);
    g_strfreev(ml);
    g_string_append_printf(o, "arquivos incluídos (%u) — só estes; o que estiver preparado fora deles continua como está:\n", p->arquivos->len);
    for (guint i = 0; i < p->arquivos->len; i++)
        g_string_append_printf(o, "  %s %s\n", (char *)g_ptr_array_index(p->estados, i), (char *)g_ptr_array_index(p->arquivos, i));
    if (g_autor_nome) g_string_append_printf(o, "autor: %s <%s>\n", g_autor_nome, g_autor_email);
    g_string_append(o, "ganchos do Git desativados · sem assinatura · sem push\n");
    /* conteúdo exato: diff do que entra (rastreados) e o texto dos novos */
    GPtrArray *a = g_ptr_array_new();
    const char *base[] = { "diff", "--no-ext-diff", "--no-textconv", "--no-color", "HEAD", "--", NULL };
    gboolean tem_head = strcmp(p->head, "(nenhum commit ainda)") != 0;
    for (int i = 0; base[i]; i++) if (tem_head || strcmp(base[i], "HEAD")) g_ptr_array_add(a, (gpointer)base[i]);
    guint rastreados = 0;
    for (guint i = 0; i < p->arquivos->len; i++)
        if (strcmp(g_ptr_array_index(p->estados, i), "??")) { g_ptr_array_add(a, g_ptr_array_index(p->arquivos, i)); rastreados++; }
    g_ptr_array_add(a, NULL);
    if (rastreados) {
        ProcResultado r;
        if (rodar((const char *const *)a->pdata, FALSE, NULL, NULL, &r)) {
            g_string_append_c(o, '\n');
            g_string_append(o, r.saida ? r.saida : "");
            if (r.stdout_truncado) g_string_append(o, "… (diff cortado na prévia; o commit inclui as alterações inteiras)\n");
        }
        proc_resultado_liberar(&r);
    }
    g_ptr_array_free(a, TRUE);
    for (guint i = 0; i < p->arquivos->len; i++) {
        if (strcmp(g_ptr_array_index(p->estados, i), "??")) continue;
        char *abs = g_build_filename(proj_raiz(), (char *)g_ptr_array_index(p->arquivos, i), NULL);
        char *c = NULL;
        gsize n = 0;
        if (g_file_get_contents(abs, &c, &n, NULL)) {
            char *u = g_utf8_validate(c, (gssize)n, NULL) && !memchr(c, 0, n) ? g_strndup(c, n) : g_strdup_printf("(arquivo binário, %zu bytes)\n", n);
            char *d = proj_diff(g_ptr_array_index(p->arquivos, i), "", u, 3, 32 * 1024);
            g_string_append_c(o, '\n');
            g_string_append(o, d);
            g_free(d); g_free(u);
        }
        g_free(c);
        g_free(abs);
    }
    plano_liberar(p);
    return g_string_free(o, FALSE);
}

char *git_commit_aplicar(JsonObject *params, volatile gint *cancelado, char **erro)
{
    PlanoCommit *p = planejar_commit(params, erro);
    if (!p) return NULL;
    GPtrArray *env = g_ptr_array_new_with_free_func(g_free);
    if (g_autor_nome) {
        g_ptr_array_add(env, g_strdup_printf("GIT_AUTHOR_NAME=%s", g_autor_nome));
        g_ptr_array_add(env, g_strdup_printf("GIT_AUTHOR_EMAIL=%s", g_autor_email));
        g_ptr_array_add(env, g_strdup_printf("GIT_COMMITTER_NAME=%s", g_autor_nome));
        g_ptr_array_add(env, g_strdup_printf("GIT_COMMITTER_EMAIL=%s", g_autor_email));
    }
    g_ptr_array_add(env, NULL);
    GPtrArray *add = g_ptr_array_new();
    const char *add_base[] = { "add", "--all", "--", NULL }; /* --all: inclui remoções dos caminhos listados */
    for (int i = 0; add_base[i]; i++) g_ptr_array_add(add, (gpointer)add_base[i]);
    for (guint i = 0; i < p->arquivos->len; i++) g_ptr_array_add(add, g_ptr_array_index(p->arquivos, i));
    g_ptr_array_add(add, NULL);
    ProcResultado r;
    char *saida = NULL;
    if (!rodar((const char *const *)add->pdata, TRUE, (char **)env->pdata, cancelado, &r)) {
        *erro = falha(&r, "git add");
        goto fim;
    }
    proc_resultado_liberar(&r);
    GPtrArray *ci = g_ptr_array_new();
    const char *ci_base[] = { "commit", "--no-verify", "--no-gpg-sign", "--only", "-m", p->mensagem, "--", NULL };
    for (int i = 0; ci_base[i]; i++) g_ptr_array_add(ci, (gpointer)ci_base[i]);
    for (guint i = 0; i < p->arquivos->len; i++) g_ptr_array_add(ci, g_ptr_array_index(p->arquivos, i));
    g_ptr_array_add(ci, NULL);
    gboolean ok = rodar((const char *const *)ci->pdata, TRUE, (char **)env->pdata, cancelado, &r);
    g_ptr_array_free(ci, TRUE);
    if (!ok) {
        *erro = falha(&r, "git commit");
        if (r.erros && strstr(r.erros, "tell me who you are")) {
            g_free(*erro);
            *erro = g_strdup("o Git não sabe quem é o autor: defina user.name/user.email no repositório ou LIA_GIT_AUTOR=\"Nome <email>\" ao iniciar a Lia");
        }
        goto fim;
    }
    proc_resultado_liberar(&r);
    const char *head_args[] = { "rev-parse", "HEAD", NULL };
    char *novo = NULL;
    if (rodar(head_args, FALSE, NULL, NULL, &r)) novo = g_strstrip(g_strdup(r.saida));
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    b_str(b, "commit", novo);
    b_str(b, "ramo", p->ramo);
    b_str(b, "mensagem", p->mensagem);
    json_builder_set_member_name(b, "arquivos");
    json_builder_begin_array(b);
    for (guint i = 0; i < p->arquivos->len; i++) json_builder_add_string_value(b, g_ptr_array_index(p->arquivos, i));
    json_builder_end_array(b);
    json_builder_end_object(b);
    saida = json_de(b);
    g_free(novo);
fim:
    proc_resultado_liberar(&r);
    g_ptr_array_free(add, TRUE);
    g_ptr_array_unref(env);
    plano_liberar(p);
    return saida;
}
