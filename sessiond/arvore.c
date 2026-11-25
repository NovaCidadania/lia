#define _GNU_SOURCE
/* Árvore (DAG) de tarefas — ver arvore.h. Módulo puro: sem daemon, sem
 * modelo; só a gravação de artefatos toca o disco. */
#include "arvore.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static const char *const NOMES[] = { "pendente", "executando", "bloqueado", "falhou", "passou" };

const char *arvore_estado_nome(NoEstado e)
{
    return (guint)e < G_N_ELEMENTS(NOMES) ? NOMES[e] : "?";
}

static gboolean estado_de_nome(const char *n, NoEstado *e)
{
    for (guint i = 0; n && i < G_N_ELEMENTS(NOMES); i++)
        if (!strcmp(n, NOMES[i])) { *e = (NoEstado)i; return TRUE; }
    return FALSE;
}

static void contexto_liberar(gpointer p)
{
    NoContexto *c = p;
    g_free(c->caminho); g_free(c->origem); g_free(c->sha256); g_free(c->trecho); g_free(c);
}

static void criterio_liberar(gpointer p)
{
    NoCriterio *c = p;
    g_free(c->descricao); g_free(c->origem); g_free(c->como); g_free(c);
}

static void evidencia_liberar(gpointer p)
{
    NoEvidencia *e = p;
    g_free(e->tipo); g_free(e->comando); g_strfreev(e->arquivos); g_strfreev(e->hashes_antes); g_strfreev(e->hashes_depois);
    g_free(e->artefato); g_free(e->alcance); g_free(e);
}

static void tentativa_liberar(gpointer p)
{
    NoTentativa *t = p;
    g_free(t->id); g_free(t->hash_proposta); g_free(t->artefato); g_free(t->prompt); g_free(t->resultado); g_free(t);
}

static void no_liberar(gpointer p)
{
    TarefaNo *n = p;
    g_free(n->id); g_free(n->pai); g_strfreev(n->deps); g_free(n->objetivo); g_strfreev(n->escopo);
    g_free(n->entradas_json); g_free(n->saidas_json); g_free(n->motivo);
    g_ptr_array_unref(n->contexto); g_ptr_array_unref(n->criterios); g_ptr_array_unref(n->evidencias); g_ptr_array_unref(n->tentativas);
    g_free(n);
}

static TarefaNo *no_novo(void)
{
    TarefaNo *n = g_new0(TarefaNo, 1);
    n->contexto = g_ptr_array_new_with_free_func(contexto_liberar);
    n->criterios = g_ptr_array_new_with_free_func(criterio_liberar);
    n->evidencias = g_ptr_array_new_with_free_func(evidencia_liberar);
    n->tentativas = g_ptr_array_new_with_free_func(tentativa_liberar);
    return n;
}

Arvore *arvore_nova(const char *id)
{
    Arvore *a = g_new0(Arvore, 1);
    a->id = g_strdup(id ? id : "");
    a->nos = g_ptr_array_new_with_free_func(no_liberar);
    a->eventos = g_ptr_array_new_with_free_func(g_free);
    return a;
}

void arvore_liberar(Arvore *a)
{
    if (!a) return;
    g_free(a->id);
    g_ptr_array_unref(a->nos);
    g_ptr_array_unref(a->eventos);
    g_free(a);
}

TarefaNo *arvore_no(Arvore *a, const char *id)
{
    for (guint i = 0; a && id && i < a->nos->len; i++) {
        TarefaNo *n = g_ptr_array_index(a->nos, i);
        if (!strcmp(n->id, id)) return n;
    }
    return NULL;
}

static gboolean id_valido(const char *id)
{
    if (!id || !*id || strlen(id) > 80) return FALSE;
    for (const char *p = id; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return FALSE;
    return TRUE;
}

static char **strv_ou_vazio(const char *const *v)
{
    return v ? g_strdupv((char **)v) : g_new0(char *, 1);
}

/* ciclo pelas arestas pai e dependências: DFS com cores */
static gboolean visitar(Arvore *a, TarefaNo *n, GHashTable *cor, GString *caminho)
{
    int c = GPOINTER_TO_INT(g_hash_table_lookup(cor, n->id));
    if (c == 1) { g_string_append_printf(caminho, "%s", n->id); return TRUE; }
    if (c == 2) return FALSE;
    g_hash_table_insert(cor, n->id, GINT_TO_POINTER(1));
    GPtrArray *viz = g_ptr_array_new();
    if (n->pai) g_ptr_array_add(viz, n->pai);
    for (int i = 0; n->deps && n->deps[i]; i++) g_ptr_array_add(viz, n->deps[i]);
    gboolean ciclo = FALSE;
    for (guint i = 0; i < viz->len && !ciclo; i++) {
        TarefaNo *m = arvore_no(a, g_ptr_array_index(viz, i));
        if (m && visitar(a, m, cor, caminho)) { g_string_prepend(caminho, " → "); g_string_prepend(caminho, n->id); ciclo = TRUE; }
    }
    g_ptr_array_free(viz, TRUE);
    g_hash_table_insert(cor, n->id, GINT_TO_POINTER(2));
    return ciclo;
}

char *arvore_validar(Arvore *a)
{
    int raizes = 0;
    GHashTable *vistos = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < a->nos->len; i++) {
        TarefaNo *n = g_ptr_array_index(a->nos, i);
        if (!id_valido(n->id)) { g_hash_table_unref(vistos); return g_strdup_printf("id de nó inválido: \"%.40s\"", n->id); }
        if (g_hash_table_contains(vistos, n->id)) { g_hash_table_unref(vistos); return g_strdup_printf("id repetido: %s", n->id); }
        g_hash_table_add(vistos, n->id);
        if (!n->pai) raizes++;
        else if (!arvore_no(a, n->pai)) { g_hash_table_unref(vistos); return g_strdup_printf("%s: pai %s não existe", n->id, n->pai); }
        for (int j = 0; n->deps && n->deps[j]; j++)
            if (!arvore_no(a, n->deps[j]) || !strcmp(n->deps[j], n->id)) {
                g_hash_table_unref(vistos);
                return g_strdup_printf("%s: dependência %s inválida", n->id, n->deps[j]);
            }
    }
    g_hash_table_unref(vistos);
    if (a->nos->len && raizes != 1) return g_strdup_printf("a árvore precisa de exatamente uma raiz (tem %d)", raizes);
    GHashTable *cor = g_hash_table_new(g_str_hash, g_str_equal);
    char *r = NULL;
    for (guint i = 0; i < a->nos->len && !r; i++) {
        GString *cam = g_string_new(NULL);
        if (visitar(a, g_ptr_array_index(a->nos, i), cor, cam)) r = g_strdup_printf("ciclo: %s", cam->str);
        g_string_free(cam, TRUE);
    }
    g_hash_table_unref(cor);
    return r;
}

char *arvore_adicionar(Arvore *a, const char *id, const char *pai, const char *const *deps, const char *objetivo, const char *const *escopo)
{
    if (!id_valido(id)) return g_strdup_printf("id de nó inválido: \"%.40s\"", id ? id : "");
    if (arvore_no(a, id)) return g_strdup_printf("id repetido: %s", id);
    if (!pai && a->nos->len) return g_strdup("só a raiz não tem pai, e a árvore já tem raiz");
    if (pai && !arvore_no(a, pai)) return g_strdup_printf("pai %s não existe", pai);
    TarefaNo *n = no_novo();
    n->id = g_strdup(id);
    n->pai = g_strdup(pai);
    n->deps = strv_ou_vazio(deps);
    n->objetivo = g_strdup(objetivo ? objetivo : "");
    n->escopo = strv_ou_vazio(escopo);
    g_ptr_array_add(a->nos, n);
    char *erro = arvore_validar(a);
    if (erro) { g_ptr_array_remove_index(a->nos, a->nos->len - 1); return erro; }
    arvore_evento(a, "no_criado", id, objetivo);
    return NULL;
}

void arvore_contexto(Arvore *a, const char *no, const char *caminho, const char *origem, const char *sha256, const char *trecho)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n || !caminho) return;
    NoContexto *c = g_new0(NoContexto, 1);
    c->caminho = g_strdup(caminho); c->origem = g_strdup(origem ? origem : ""); c->sha256 = g_strdup(sha256 ? sha256 : "");
    c->trecho = g_strdup(trecho ? trecho : "");
    g_ptr_array_add(n->contexto, c);
}

guint arvore_criterio(Arvore *a, const char *no, const char *descricao, const char *origem)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n) return G_MAXUINT;
    NoCriterio *c = g_new0(NoCriterio, 1);
    c->descricao = g_strdup(descricao ? descricao : "");
    c->origem = g_strdup(origem ? origem : "pedido");
    g_ptr_array_add(n->criterios, c);
    return n->criterios->len - 1;
}

void arvore_conferir(Arvore *a, const char *no, guint i, gboolean ok, const char *como)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n || i >= n->criterios->len) return;
    NoCriterio *c = g_ptr_array_index(n->criterios, i);
    c->conferido = TRUE; c->ok = ok;
    g_free(c->como); c->como = g_strdup(como ? como : "");
}

char *arvore_tentativa(Arvore *a, const char *no, const char *hash_proposta, const char *artefato)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n) return g_strdup_printf("nó %s não existe", no ? no : "");
    if (n->max_tentativas > 0 && (int)n->tentativas->len >= n->max_tentativas)
        return g_strdup_printf("orçamento de tentativas de %s esgotado (%d)", n->id, n->max_tentativas);
    for (guint i = 0; hash_proposta && i < n->tentativas->len; i++) {
        NoTentativa *t = g_ptr_array_index(n->tentativas, i);
        if (t->hash_proposta && !strcmp(t->hash_proposta, hash_proposta))
            return g_strdup_printf("repetição sem progresso: a tentativa repete exatamente %s (%s)", t->id, t->resultado ? t->resultado : "?");
    }
    NoTentativa *t = g_new0(NoTentativa, 1);
    t->id = g_strdup_printf("%s.t%u", n->id, n->tentativas->len + 1);
    t->seq = ++a->seq;
    t->hash_proposta = g_strdup(hash_proposta);
    t->artefato = g_strdup(artefato);
    t->resultado = g_strdup("pendente");
    g_ptr_array_add(n->tentativas, t);
    arvore_evento(a, "tentativa", n->id, t->id);
    return NULL;
}

void arvore_tentativa_resultado(Arvore *a, const char *no, const char *resultado)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n || !n->tentativas->len) return;
    NoTentativa *t = g_ptr_array_index(n->tentativas, n->tentativas->len - 1);
    g_free(t->resultado);
    t->resultado = g_strdup(resultado ? resultado : "");
    arvore_evento(a, "tentativa_resultado", n->id, resultado);
}

void arvore_tentativa_prompt(Arvore *a, const char *no, const char *sha_prompt)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n || !n->tentativas->len) return;
    NoTentativa *t = g_ptr_array_index(n->tentativas, n->tentativas->len - 1);
    g_free(t->prompt);
    t->prompt = g_strdup(sha_prompt);
}

gboolean arvore_consumir(Arvore *a, const char *no, gint64 ms, gint64 tokens)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n) return FALSE;
    n->ms_usados += MAX(ms, 0);
    n->tokens_usados += MAX(tokens, 0);
    return !((n->max_ms > 0 && n->ms_usados > n->max_ms) || (n->max_tokens > 0 && n->tokens_usados > n->max_tokens));
}

static char *curto(const char *h)
{
    return g_strndup(h && *h ? h : "(novo)", h && *h ? 12 : 6);
}

gint64 arvore_patch(Arvore *a, const char *no, char **arquivos, char **antes, char **depois, const char *artefato)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n) return -1;
    NoEvidencia *e = g_new0(NoEvidencia, 1);
    e->tipo = g_strdup("patch");
    e->seq = ++a->seq;
    e->arquivos = strv_ou_vazio((const char *const *)arquivos);
    e->hashes_antes = strv_ou_vazio((const char *const *)antes);
    e->hashes_depois = strv_ou_vazio((const char *const *)depois);
    e->artefato = g_strdup(artefato);
    e->passou = TRUE;
    GString *al = g_string_new("patch gravado: ");
    for (int i = 0; e->arquivos[i]; i++) {
        char *x = curto(i < (int)g_strv_length(e->hashes_antes) ? e->hashes_antes[i] : NULL);
        char *y = curto(i < (int)g_strv_length(e->hashes_depois) ? e->hashes_depois[i] : NULL);
        g_string_append_printf(al, "%s%s %s→%s", i ? ", " : "", e->arquivos[i], x, y);
        g_free(x); g_free(y);
    }
    g_string_append_printf(al, " (#%" G_GINT64_FORMAT "); prova só que o arquivo mudou, não que está certo", e->seq);
    e->alcance = g_string_free(al, FALSE);
    g_ptr_array_add(n->evidencias, e);
    arvore_evento(a, "patch", n->id, e->alcance);
    return e->seq;
}

/* último patch na subárvore de 'n' (n e descendentes) */
static gint64 ultimo_patch(Arvore *a, TarefaNo *n)
{
    gint64 m = 0;
    for (guint i = 0; i < n->evidencias->len; i++) {
        NoEvidencia *e = g_ptr_array_index(n->evidencias, i);
        if (!strcmp(e->tipo, "patch")) m = MAX(m, e->seq);
    }
    for (guint i = 0; i < a->nos->len; i++) {
        TarefaNo *f = g_ptr_array_index(a->nos, i);
        if (f->pai && !strcmp(f->pai, n->id)) m = MAX(m, ultimo_patch(a, f));
    }
    return m;
}

gint64 arvore_validacao(Arvore *a, const char *no, const char *comando, int codigo_saida, gboolean passou,
                        char **arquivos, char **hashes, const char *artefato)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n) return -1;
    gint64 up = ultimo_patch(a, n);
    NoEvidencia *e = g_new0(NoEvidencia, 1);
    e->tipo = g_strdup("validacao");
    e->seq = ++a->seq;
    e->comando = g_strdup(comando ? comando : "?");
    e->codigo_saida = codigo_saida;
    e->passou = passou;
    e->arquivos = strv_ou_vazio((const char *const *)arquivos);
    e->hashes_depois = strv_ou_vazio((const char *const *)hashes);
    e->hashes_antes = g_new0(char *, 1);
    e->artefato = g_strdup(artefato);
    GString *al = g_string_new(NULL);
    g_string_append_printf(al, "validação \"%s\" %s (código %d)", e->comando, passou ? "passou" : "falhou", codigo_saida);
    if (e->arquivos[0]) {
        g_string_append(al, " com ");
        for (int i = 0; e->arquivos[i]; i++) {
            char *h = curto(i < (int)g_strv_length(e->hashes_depois) ? e->hashes_depois[i] : NULL);
            g_string_append_printf(al, "%s%s@%s", i ? ", " : "", e->arquivos[i], h);
            g_free(h);
        }
    }
    if (up) g_string_append_printf(al, ", depois do patch #%" G_GINT64_FORMAT, up);
    else g_string_append(al, ", sem patch antes");
    g_string_append(al, "; vale só para esses arquivos/hashes e esse comando");
    e->alcance = g_string_free(al, FALSE);
    g_ptr_array_add(n->evidencias, e);
    arvore_evento(a, "validacao", n->id, e->alcance);
    return e->seq;
}

NoEstado arvore_avaliar(Arvore *a, const char *no, char **motivo)
{
    TarefaNo *n = arvore_no(a, no);
    char *m = NULL;
    NoEstado r = NO_PASSOU;
    if (!n) { if (motivo) *motivo = g_strdup("nó inexistente"); return NO_FALHOU; }
    for (int i = 0; n->deps && n->deps[i] && !m; i++) {
        TarefaNo *d = arvore_no(a, n->deps[i]);
        if (!d || d->estado != NO_PASSOU) { r = NO_BLOQUEADO; m = g_strdup_printf("dependência %s não passou", n->deps[i]); }
    }
    for (guint i = 0; !m && i < n->criterios->len; i++) {
        NoCriterio *c = g_ptr_array_index(n->criterios, i);
        if (!c->conferido) { r = NO_FALHOU; m = g_strdup_printf("critério não conferido: %s", c->descricao); }
        else if (!c->ok) { r = NO_FALHOU; m = g_strdup_printf("critério não atendido: %s", c->descricao); }
    }
    if (!m) {
        gint64 up = ultimo_patch(a, n);
        /* só a ÚLTIMA validação do próprio nó conta, e ela precisa ter
         * passado depois do último patch da subárvore */
        gint64 ultima = 0;
        gboolean ultima_passou = FALSE;
        for (guint i = 0; i < n->evidencias->len; i++) {
            NoEvidencia *e = g_ptr_array_index(n->evidencias, i);
            if (!strcmp(e->tipo, "validacao") && e->seq > ultima) { ultima = e->seq; ultima_passou = e->passou; }
        }
        if (!ultima) { r = NO_FALHOU; m = g_strdup("nenhuma validação própria deste nó"); }
        else if (!ultima_passou) { r = NO_FALHOU; m = g_strdup("a última validação deste nó falhou"); }
        else if (ultima < up) { r = NO_FALHOU; m = g_strdup_printf("a validação (#%" G_GINT64_FORMAT ") é anterior ao último patch da subárvore (#%" G_GINT64_FORMAT ")", ultima, up); }
        else if (!up) { r = NO_FALHOU; m = g_strdup("nenhum patch na subárvore: nada a provar"); }
    }
    if (!m) m = g_strdup("critérios próprios conferidos e validação própria passou depois do último patch da subárvore");
    n->estado = r;
    g_free(n->motivo);
    n->motivo = g_strdup(m);
    arvore_evento(a, "avaliacao", n->id, m);
    if (motivo) *motivo = m; else g_free(m);
    return r;
}

void arvore_marcar(Arvore *a, const char *no, NoEstado estado, const char *motivo)
{
    TarefaNo *n = arvore_no(a, no);
    if (!n) return;
    n->estado = estado;
    g_free(n->motivo);
    n->motivo = g_strdup(motivo);
    arvore_evento(a, arvore_estado_nome(estado), no, motivo);
}

/* ---------------------------------------------------------------- JSON */

static void add_strv(JsonBuilder *b, const char *nome, char **v)
{
    json_builder_set_member_name(b, nome);
    json_builder_begin_array(b);
    for (int i = 0; v && v[i]; i++) json_builder_add_string_value(b, v[i]);
    json_builder_end_array(b);
}

static void add_str(JsonBuilder *b, const char *nome, const char *v)
{
    json_builder_set_member_name(b, nome);
    if (v) json_builder_add_string_value(b, v); else json_builder_add_null_value(b);
}

char *arvore_para_json(Arvore *a)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    add_str(b, "versao", ARVORE_VERSAO);
    add_str(b, "id", a->id);
    json_builder_set_member_name(b, "seq"); json_builder_add_int_value(b, a->seq);
    json_builder_set_member_name(b, "nos");
    json_builder_begin_array(b);
    for (guint i = 0; i < a->nos->len; i++) {
        TarefaNo *n = g_ptr_array_index(a->nos, i);
        json_builder_begin_object(b);
        add_str(b, "id", n->id); add_str(b, "pai", n->pai); add_strv(b, "deps", n->deps);
        add_str(b, "objetivo", n->objetivo); add_strv(b, "escopo", n->escopo);
        add_str(b, "entradas", n->entradas_json); add_str(b, "saidas", n->saidas_json);
        add_str(b, "estado", arvore_estado_nome(n->estado)); add_str(b, "motivo", n->motivo);
        json_builder_set_member_name(b, "orcamento");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "max_tentativas"); json_builder_add_int_value(b, n->max_tentativas);
        json_builder_set_member_name(b, "max_ms"); json_builder_add_int_value(b, n->max_ms);
        json_builder_set_member_name(b, "max_tokens"); json_builder_add_int_value(b, n->max_tokens);
        json_builder_set_member_name(b, "ms_usados"); json_builder_add_int_value(b, n->ms_usados);
        json_builder_set_member_name(b, "tokens_usados"); json_builder_add_int_value(b, n->tokens_usados);
        json_builder_end_object(b);
        json_builder_set_member_name(b, "contexto");
        json_builder_begin_array(b);
        for (guint j = 0; j < n->contexto->len; j++) {
            NoContexto *c = g_ptr_array_index(n->contexto, j);
            json_builder_begin_object(b);
            add_str(b, "caminho", c->caminho); add_str(b, "origem", c->origem); add_str(b, "sha256", c->sha256); add_str(b, "trecho", c->trecho);
            json_builder_end_object(b);
        }
        json_builder_end_array(b);
        json_builder_set_member_name(b, "criterios");
        json_builder_begin_array(b);
        for (guint j = 0; j < n->criterios->len; j++) {
            NoCriterio *c = g_ptr_array_index(n->criterios, j);
            json_builder_begin_object(b);
            add_str(b, "descricao", c->descricao); add_str(b, "origem", c->origem);
            json_builder_set_member_name(b, "conferido"); json_builder_add_boolean_value(b, c->conferido);
            json_builder_set_member_name(b, "ok"); json_builder_add_boolean_value(b, c->ok);
            add_str(b, "como", c->como);
            json_builder_end_object(b);
        }
        json_builder_end_array(b);
        json_builder_set_member_name(b, "evidencias");
        json_builder_begin_array(b);
        for (guint j = 0; j < n->evidencias->len; j++) {
            NoEvidencia *e = g_ptr_array_index(n->evidencias, j);
            json_builder_begin_object(b);
            add_str(b, "tipo", e->tipo);
            json_builder_set_member_name(b, "seq"); json_builder_add_int_value(b, e->seq);
            add_str(b, "comando", e->comando);
            json_builder_set_member_name(b, "codigo_saida"); json_builder_add_int_value(b, e->codigo_saida);
            json_builder_set_member_name(b, "passou"); json_builder_add_boolean_value(b, e->passou);
            add_strv(b, "arquivos", e->arquivos); add_strv(b, "hashes_antes", e->hashes_antes); add_strv(b, "hashes_depois", e->hashes_depois);
            add_str(b, "artefato", e->artefato); add_str(b, "alcance", e->alcance);
            json_builder_end_object(b);
        }
        json_builder_end_array(b);
        json_builder_set_member_name(b, "tentativas");
        json_builder_begin_array(b);
        for (guint j = 0; j < n->tentativas->len; j++) {
            NoTentativa *t = g_ptr_array_index(n->tentativas, j);
            json_builder_begin_object(b);
            add_str(b, "id", t->id);
            json_builder_set_member_name(b, "seq"); json_builder_add_int_value(b, t->seq);
            add_str(b, "hash_proposta", t->hash_proposta); add_str(b, "artefato", t->artefato); add_str(b, "prompt", t->prompt);
            add_str(b, "resultado", t->resultado);
            json_builder_end_object(b);
        }
        json_builder_end_array(b);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    JsonGenerator *g = json_generator_new();
    JsonNode *raiz = json_builder_get_root(b);
    json_generator_set_root(g, raiz);
    json_generator_set_pretty(g, TRUE);
    char *t = json_generator_to_data(g, NULL);
    json_node_unref(raiz);
    g_object_unref(g);
    g_object_unref(b);
    return t;
}

static const char *js(JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return n && JSON_NODE_TYPE(n) == JSON_NODE_VALUE && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}

static gint64 ji(JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return n && JSON_NODE_TYPE(n) == JSON_NODE_VALUE && json_node_get_value_type(n) == G_TYPE_INT64 ? json_node_get_int(n) : 0;
}

static gboolean jb(JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return n && JSON_NODE_TYPE(n) == JSON_NODE_VALUE && json_node_get_value_type(n) == G_TYPE_BOOLEAN && json_node_get_boolean(n);
}

static char **jv(JsonObject *o, const char *k)
{
    GPtrArray *v = g_ptr_array_new();
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    if (n && JSON_NODE_TYPE(n) == JSON_NODE_ARRAY) {
        JsonArray *a = json_node_get_array(n);
        for (guint i = 0; i < json_array_get_length(a); i++) {
            JsonNode *e = json_array_get_element(a, i);
            if (JSON_NODE_TYPE(e) == JSON_NODE_VALUE && json_node_get_value_type(e) == G_TYPE_STRING) g_ptr_array_add(v, g_strdup(json_node_get_string(e)));
        }
    }
    g_ptr_array_add(v, NULL);
    return (char **)g_ptr_array_free(v, FALSE);
}

static JsonArray *jarr(JsonObject *o, const char *k)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return n && JSON_NODE_TYPE(n) == JSON_NODE_ARRAY ? json_node_get_array(n) : NULL;
}

static JsonObject *jobj_i(JsonArray *a, guint i)
{
    JsonNode *n = json_array_get_element(a, i);
    return JSON_NODE_TYPE(n) == JSON_NODE_OBJECT ? json_node_get_object(n) : NULL;
}

Arvore *arvore_de_json(const char *texto, char **erro)
{
    JsonParser *p = json_parser_new();
    if (!texto || !json_parser_load_from_data(p, texto, -1, NULL) || JSON_NODE_TYPE(json_parser_get_root(p)) != JSON_NODE_OBJECT) {
        g_object_unref(p);
        *erro = g_strdup("não é um objeto JSON");
        return NULL;
    }
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    if (g_strcmp0(js(o, "versao"), ARVORE_VERSAO)) {
        *erro = g_strdup_printf("versão \"%s\" desconhecida (esperada %s)", js(o, "versao") ? js(o, "versao") : "", ARVORE_VERSAO);
        g_object_unref(p);
        return NULL;
    }
    Arvore *a = arvore_nova(js(o, "id"));
    a->seq = ji(o, "seq");
    JsonArray *nos = jarr(o, "nos");
    for (guint i = 0; nos && i < json_array_get_length(nos); i++) {
        JsonObject *no = jobj_i(nos, i);
        if (!no) continue;
        TarefaNo *n = no_novo();
        n->id = g_strdup(js(no, "id") ? js(no, "id") : "");
        n->pai = g_strdup(js(no, "pai"));
        n->deps = jv(no, "deps");
        n->objetivo = g_strdup(js(no, "objetivo") ? js(no, "objetivo") : "");
        n->escopo = jv(no, "escopo");
        n->entradas_json = g_strdup(js(no, "entradas"));
        n->saidas_json = g_strdup(js(no, "saidas"));
        if (!estado_de_nome(js(no, "estado"), &n->estado)) n->estado = NO_PENDENTE;
        n->motivo = g_strdup(js(no, "motivo"));
        JsonNode *on = json_object_get_member(no, "orcamento");
        JsonObject *orc = on && JSON_NODE_TYPE(on) == JSON_NODE_OBJECT ? json_node_get_object(on) : NULL;
        n->max_tentativas = (int)ji(orc, "max_tentativas"); n->max_ms = ji(orc, "max_ms"); n->max_tokens = ji(orc, "max_tokens");
        n->ms_usados = ji(orc, "ms_usados"); n->tokens_usados = ji(orc, "tokens_usados");
        JsonArray *x = jarr(no, "contexto");
        for (guint j = 0; x && j < json_array_get_length(x); j++) {
            JsonObject *c = jobj_i(x, j);
            if (c) { NoContexto *nc = g_new0(NoContexto, 1); nc->caminho = g_strdup(js(c, "caminho")); nc->origem = g_strdup(js(c, "origem"));
                     nc->sha256 = g_strdup(js(c, "sha256")); nc->trecho = g_strdup(js(c, "trecho")); g_ptr_array_add(n->contexto, nc); }
        }
        x = jarr(no, "criterios");
        for (guint j = 0; x && j < json_array_get_length(x); j++) {
            JsonObject *c = jobj_i(x, j);
            if (c) { NoCriterio *nc = g_new0(NoCriterio, 1); nc->descricao = g_strdup(js(c, "descricao")); nc->origem = g_strdup(js(c, "origem"));
                     nc->conferido = jb(c, "conferido"); nc->ok = jb(c, "ok"); nc->como = g_strdup(js(c, "como")); g_ptr_array_add(n->criterios, nc); }
        }
        x = jarr(no, "evidencias");
        for (guint j = 0; x && j < json_array_get_length(x); j++) {
            JsonObject *c = jobj_i(x, j);
            if (!c) continue;
            NoEvidencia *e = g_new0(NoEvidencia, 1);
            e->tipo = g_strdup(js(c, "tipo") ? js(c, "tipo") : "?"); e->seq = ji(c, "seq"); e->comando = g_strdup(js(c, "comando"));
            e->codigo_saida = (int)ji(c, "codigo_saida"); e->passou = jb(c, "passou");
            e->arquivos = jv(c, "arquivos"); e->hashes_antes = jv(c, "hashes_antes"); e->hashes_depois = jv(c, "hashes_depois");
            e->artefato = g_strdup(js(c, "artefato")); e->alcance = g_strdup(js(c, "alcance"));
            g_ptr_array_add(n->evidencias, e);
        }
        x = jarr(no, "tentativas");
        for (guint j = 0; x && j < json_array_get_length(x); j++) {
            JsonObject *c = jobj_i(x, j);
            if (!c) continue;
            NoTentativa *t = g_new0(NoTentativa, 1);
            t->id = g_strdup(js(c, "id")); t->seq = ji(c, "seq"); t->hash_proposta = g_strdup(js(c, "hash_proposta"));
            t->artefato = g_strdup(js(c, "artefato")); t->prompt = g_strdup(js(c, "prompt")); t->resultado = g_strdup(js(c, "resultado"));
            g_ptr_array_add(n->tentativas, t);
        }
        g_ptr_array_add(a->nos, n);
    }
    g_object_unref(p);
    char *e = arvore_validar(a);
    if (e) { *erro = e; arvore_liberar(a); return NULL; }
    return a;
}

void arvore_evento(Arvore *a, const char *tipo, const char *no, const char *detalhe)
{
    if (!a) return;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    add_str(b, "versao", ARVORE_VERSAO);
    json_builder_set_member_name(b, "seq"); json_builder_add_int_value(b, a->seq);
    GDateTime *agora = g_date_time_new_now_local();
    char *q = g_date_time_format_iso8601(agora);
    add_str(b, "quando", q);
    g_free(q);
    g_date_time_unref(agora);
    add_str(b, "tipo", tipo); add_str(b, "no", no); add_str(b, "detalhe", detalhe);
    json_builder_end_object(b);
    JsonGenerator *g = json_generator_new();
    JsonNode *r = json_builder_get_root(b);
    json_generator_set_root(g, r);
    g_ptr_array_add(a->eventos, json_generator_to_data(g, NULL));
    json_node_unref(r);
    g_object_unref(g);
    g_object_unref(b);
}

char *arvore_eventos_pendentes(Arvore *a)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; a && i < a->eventos->len; i++) { g_string_append(s, g_ptr_array_index(a->eventos, i)); g_string_append_c(s, '\n'); }
    if (a) g_ptr_array_set_size(a->eventos, 0);
    return g_string_free(s, FALSE);
}

char *artefato_gravar(const char *dir, const char *conteudo, gsize tamanho, char **erro)
{
    char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)(conteudo ? conteudo : ""), conteudo ? tamanho : 0);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        *erro = g_strdup_printf("não foi possível criar %s: %s", dir, g_strerror(errno));
        g_free(sha);
        return NULL;
    }
    char *cam = g_build_filename(dir, sha, NULL);
    int fd = open(cam, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 && errno == EEXIST) { g_free(cam); return sha; } /* mesmo conteúdo: já está lá, nunca sobrescreve */
    if (fd < 0) { *erro = g_strdup_printf("não foi possível gravar %s: %s", cam, g_strerror(errno)); g_free(cam); g_free(sha); return NULL; }
    gsize feito = 0;
    while (feito < tamanho) {
        ssize_t w = write(fd, conteudo + feito, tamanho - feito);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { *erro = g_strdup_printf("falha ao gravar %s", cam); close(fd); g_unlink(cam); g_free(cam); g_free(sha); return NULL; }
        feito += (gsize)w;
    }
    close(fd);
    g_free(cam);
    return sha;
}
