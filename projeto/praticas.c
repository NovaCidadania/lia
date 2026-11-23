/* Catálogo de boas práticas — ver projeto/praticas.h. */
#include "praticas.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct PraticasCatalogo {
    GPtrArray *praticas; /* Pratica* (possui) */
    char *versao;
};

static void pratica_liberar(gpointer p_)
{
    Pratica *p = p_;
    if (!p) return;
    g_free(p->id); g_free(p->categoria); g_strfreev(p->linguagens); g_strfreev(p->ativacao); g_free(p->regra);
    g_strfreev(p->passos); g_strfreev(p->checagens); g_strfreev(p->anti_padroes); g_free(p->severidade);
    g_strfreev(p->referencias); g_free(p->origem_arquivo);
    g_free(p);
}

void praticas_liberar(PraticasCatalogo *c)
{
    if (!c) return;
    g_ptr_array_unref(c->praticas);
    g_free(c->versao);
    g_free(c);
}

static PraticasCatalogo *g_catalogo;
void praticas_definir_global(PraticasCatalogo *c) { g_catalogo = c; }
const PraticasCatalogo *praticas_global(void) { return g_catalogo; }

guint praticas_total(const PraticasCatalogo *c) { return c ? c->praticas->len : 0; }
const char *praticas_versao(const PraticasCatalogo *c) { return c ? c->versao : NULL; }

const Pratica *praticas_buscar_id(const PraticasCatalogo *c, const char *id)
{
    for (guint i = 0; c && id && i < c->praticas->len; i++) {
        Pratica *p = g_ptr_array_index(c->praticas, i);
        if (!strcmp(p->id, id)) return p;
    }
    return NULL;
}

/* minúsculas, sem acento, só letras/dígitos/'.'/'+'/'#' e espaços simples, com espaço nas pontas */
static char *normalizar(const char *s)
{
    char *ascii = g_str_to_ascii(s ? s : "", "C");
    GString *o = g_string_new(" ");
    for (const char *p = ascii; *p; p++) {
        char c = g_ascii_tolower(*p);
        gboolean ok = g_ascii_isalnum(c) || c == '.' || c == '+' || c == '#' || c == '_' || c == '-';
        if (ok) g_string_append_c(o, c);
        else if (o->str[o->len - 1] != ' ') g_string_append_c(o, ' ');
    }
    if (o->str[o->len - 1] != ' ') g_string_append_c(o, ' ');
    g_free(ascii);
    return g_string_free(o, FALSE);
}

/* texto de prática: sem caracteres de controle, tamanho limitado */
static gboolean texto_valido(const char *s)
{
    if (!s || !*s || strlen(s) > PRATICAS_MAX_TEXTO || !g_utf8_validate(s, -1, NULL)) return FALSE;
    for (const char *p = s; *p; p++) if ((unsigned char)*p < 0x20) return FALSE;
    return TRUE;
}

static char **lista_strings(JsonObject *o, const char *k, gboolean normalizar_termos, gboolean pode_vazia, char **erro, const char *id)
{
    if (!json_object_has_member(o, k) || JSON_NODE_TYPE(json_object_get_member(o, k)) != JSON_NODE_ARRAY) {
        *erro = g_strdup_printf("prática '%s': '%s' precisa ser uma lista de textos", id, k);
        return NULL;
    }
    JsonArray *a = json_object_get_array_member(o, k);
    guint n = json_array_get_length(a);
    if (!n && !pode_vazia) { *erro = g_strdup_printf("prática '%s': '%s' não pode ser vazia", id, k); return NULL; }
    if (n > 40) { *erro = g_strdup_printf("prática '%s': '%s' tem itens demais (%u)", id, k, n); return NULL; }
    char **v = g_new0(char *, n + 1);
    for (guint i = 0; i < n; i++) {
        JsonNode *e = json_array_get_element(a, i);
        const char *s = JSON_NODE_TYPE(e) == JSON_NODE_VALUE && json_node_get_value_type(e) == G_TYPE_STRING ? json_node_get_string(e) : NULL;
        if (!texto_valido(s)) {
            *erro = g_strdup_printf("prática '%s': item %u de '%s' inválido (vazio, grande demais, não UTF-8 ou com caractere de controle)", id, i + 1, k);
            g_strfreev(v);
            return NULL;
        }
        if (normalizar_termos) { char *t = normalizar(s); v[i] = g_strdup(g_strstrip(t)); g_free(t); }
        else if (!strcmp(k, "linguagens")) v[i] = g_ascii_strdown(s, -1); /* "*" continua "*" */
        else v[i] = g_strdup(s);
    }
    return v;
}

static const char *str_obrig(JsonObject *o, const char *k, const char *id, char **erro)
{
    const char *s = json_object_has_member(o, k) && JSON_NODE_TYPE(json_object_get_member(o, k)) == JSON_NODE_VALUE &&
                    json_node_get_value_type(json_object_get_member(o, k)) == G_TYPE_STRING ? json_object_get_string_member(o, k) : NULL;
    if (!texto_valido(s)) { *erro = g_strdup_printf("prática '%s': '%s' ausente ou inválido", id ? id : "?", k); return NULL; }
    return s;
}

static gboolean int_obrig(JsonObject *o, const char *k, gint64 min, gint64 max, gint64 *v, const char *id, char **erro)
{
    if (!json_object_has_member(o, k) || JSON_NODE_TYPE(json_object_get_member(o, k)) != JSON_NODE_VALUE ||
        json_node_get_value_type(json_object_get_member(o, k)) != G_TYPE_INT64) {
        *erro = g_strdup_printf("prática '%s': '%s' precisa ser inteiro", id, k);
        return FALSE;
    }
    *v = json_object_get_int_member(o, k);
    if (*v < min || *v > max) { *erro = g_strdup_printf("prática '%s': '%s' fora da faixa %" G_GINT64_FORMAT "..%" G_GINT64_FORMAT, id, k, min, max); return FALSE; }
    return TRUE;
}

static Pratica *ler_pratica(JsonObject *o, const char *arquivo, char **erro)
{
    const char *id = str_obrig(o, "id", NULL, erro);
    if (!id) return NULL;
    for (const char *q = id; *q; q++)
        if (!g_ascii_isalnum(*q) && !strchr(".-_", *q)) { *erro = g_strdup_printf("prática '%s': id só pode ter letras, dígitos, '.', '-' e '_'", id); return NULL; }
    Pratica *p = g_new0(Pratica, 1);
    p->id = g_strdup(id);
    p->origem_arquivo = g_strdup(arquivo);
    gint64 v = 0;
    const char *s;
    if (!int_obrig(o, "versao", 1, 1000000, &v, id, erro)) goto falha;
    p->versao = (int)v;
    if (!int_obrig(o, "prioridade", 0, 100, &v, id, erro)) goto falha;
    p->prioridade = (int)v;
    if (!(s = str_obrig(o, "categoria", id, erro))) goto falha;
    p->categoria = g_strdup(s);
    if (!(s = str_obrig(o, "regra", id, erro))) goto falha;
    p->regra = g_strdup(s);
    if (!(s = str_obrig(o, "severidade", id, erro))) goto falha;
    if (strcmp(s, "obrigatoria") && strcmp(s, "recomendada")) { *erro = g_strdup_printf("prática '%s': severidade deve ser obrigatoria ou recomendada", id); goto falha; }
    p->severidade = g_strdup(s);
    if (!(p->linguagens = lista_strings(o, "linguagens", FALSE, FALSE, erro, id))) goto falha;
    if (!(p->ativacao = lista_strings(o, "ativacao", TRUE, FALSE, erro, id))) goto falha;
    if (!(p->passos = lista_strings(o, "passos", FALSE, FALSE, erro, id))) goto falha;
    if (!(p->checagens = lista_strings(o, "checagens", FALSE, FALSE, erro, id))) goto falha;
    if (!(p->anti_padroes = lista_strings(o, "anti_padroes", FALSE, TRUE, erro, id))) goto falha;
    if (!(p->referencias = lista_strings(o, "referencias", FALSE, TRUE, erro, id))) goto falha;
    return p;
falha:
    pratica_liberar(p);
    return NULL;
}

PraticasCatalogo *praticas_carregar(const char *const *arquivos, char **erro)
{
    PraticasCatalogo *c = g_new0(PraticasCatalogo, 1);
    c->praticas = g_ptr_array_new_with_free_func(pratica_liberar);
    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA256);
    for (int i = 0; arquivos && arquivos[i]; i++) {
        char *dados = NULL;
        gsize n = 0;
        if (!g_file_get_contents(arquivos[i], &dados, &n, NULL)) {
            if (i == 0) { *erro = g_strdup_printf("catálogo de práticas não encontrado: %s", arquivos[i]); goto falha; }
            continue; /* extensões são opcionais */
        }
        g_checksum_update(ck, (const guchar *)dados, (gssize)n);
        JsonParser *jp = json_parser_new();
        GError *ge = NULL;
        if (!json_parser_load_from_data(jp, dados, (gssize)n, &ge) || JSON_NODE_TYPE(json_parser_get_root(jp)) != JSON_NODE_OBJECT) {
            *erro = g_strdup_printf("%s: JSON inválido%s%s", arquivos[i], ge ? ": " : "", ge ? ge->message : "");
            if (ge) g_error_free(ge);
            g_object_unref(jp); g_free(dados);
            goto falha;
        }
        g_free(dados);
        JsonObject *raiz = json_node_get_object(json_parser_get_root(jp));
        if (!json_object_has_member(raiz, "schema_version") || json_object_get_int_member(raiz, "schema_version") != 2 ||
            !json_object_has_member(raiz, "praticas") || JSON_NODE_TYPE(json_object_get_member(raiz, "praticas")) != JSON_NODE_ARRAY) {
            *erro = g_strdup_printf("%s: esperado {\"schema_version\": 2, \"praticas\": [...]}", arquivos[i]);
            g_object_unref(jp);
            goto falha;
        }
        JsonArray *a = json_object_get_array_member(raiz, "praticas");
        GHashTable *ids_arquivo = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint k = 0; k < json_array_get_length(a); k++) {
            JsonNode *e = json_array_get_element(a, k);
            if (JSON_NODE_TYPE(e) != JSON_NODE_OBJECT) { *erro = g_strdup_printf("%s: item %u não é objeto", arquivos[i], k + 1); g_hash_table_unref(ids_arquivo); g_object_unref(jp); goto falha; }
            char *e2 = NULL;
            Pratica *p = ler_pratica(json_node_get_object(e), arquivos[i], &e2);
            if (!p) { *erro = g_strdup_printf("%s: %s", arquivos[i], e2); g_free(e2); g_hash_table_unref(ids_arquivo); g_object_unref(jp); goto falha; }
            if (g_hash_table_contains(ids_arquivo, p->id)) {
                *erro = g_strdup_printf("%s: id repetido '%s'", arquivos[i], p->id);
                pratica_liberar(p); g_hash_table_unref(ids_arquivo); g_object_unref(jp);
                goto falha;
            }
            g_hash_table_add(ids_arquivo, p->id);
            /* extensão do projeto: o mesmo id substitui a do núcleo */
            gboolean trocou = FALSE;
            for (guint q = 0; q < c->praticas->len && !trocou; q++)
                if (!strcmp(((Pratica *)g_ptr_array_index(c->praticas, q))->id, p->id)) {
                    pratica_liberar(c->praticas->pdata[q]);
                    c->praticas->pdata[q] = p;
                    trocou = TRUE;
                }
            if (!trocou) g_ptr_array_add(c->praticas, p);
        }
        g_hash_table_unref(ids_arquivo);
        g_object_unref(jp);
    }
    c->versao = g_strdup(g_checksum_get_string(ck));
    g_checksum_free(ck);
    return c;
falha:
    g_checksum_free(ck);
    praticas_liberar(c);
    return NULL;
}

const char *praticas_linguagem_da_tarefa(const char *tarefa)
{
    char *t = normalizar(tarefa);
    const struct { const char *const *termos; const char *ling; } MAPA[] = {
        { (const char *const[]){ " c ", ".c ", ".h ", " em c ", " linguagem c ", " gcc ", " clang ", " malloc", " ponteiro", " struct ", NULL }, "c" },
        { (const char *const[]){ " python", ".py ", " pip ", NULL }, "python" },
        { (const char *const[]){ " shell", " bash", " sh ", ".sh ", " script de shell", " zsh", NULL }, "shell" },
        { (const char *const[]){ " sql", " postgres", " sqlite", " mysql", " banco de dados", NULL }, "sql" },
        { (const char *const[]){ " javascript", " typescript", ".js ", ".ts ", " node", NULL }, "javascript" },
        { (const char *const[]){ " rust", ".rs ", " cargo ", NULL }, "rust" },
        { (const char *const[]){ " go ", " golang", ".go ", NULL }, "go" },
        { (const char *const[]){ " haskell", ".hs ", NULL }, "haskell" },
        { NULL, NULL },
    };
    const char *r = NULL;
    for (int i = 0; MAPA[i].termos && !r; i++)
        for (int k = 0; MAPA[i].termos[k] && !r; k++)
            if (strstr(t, MAPA[i].termos[k])) r = MAPA[i].ling;
    g_free(t);
    return r;
}

typedef struct { const Pratica *p; int pontos; } Candidata;

static gint cmp_candidata(gconstpointer a, gconstpointer b)
{
    const Candidata *x = a, *y = b;
    if (x->pontos != y->pontos) return y->pontos - x->pontos;
    if (x->p->prioridade != y->p->prioridade) return y->p->prioridade - x->p->prioridade;
    return strcmp(x->p->id, y->p->id);
}

static gsize tamanho_pratica(const Pratica *p)
{
    gsize n = strlen(p->id) + strlen(p->regra) + 16;
    for (int i = 0; p->passos[i]; i++) n += strlen(p->passos[i]) + 4;
    for (int i = 0; p->checagens[i]; i++) n += strlen(p->checagens[i]) + 4;
    return n;
}

GPtrArray *praticas_selecionar(const PraticasCatalogo *c, const char *tarefa, const char *linguagem, int max, gsize max_bytes)
{
    GPtrArray *sel = g_ptr_array_new();
    if (!c || !tarefa || !*tarefa) return sel;
    if (max <= 0 || max > PRATICAS_MAX_SELECAO) max = PRATICAS_MAX_SELECAO;
    if (!linguagem) linguagem = praticas_linguagem_da_tarefa(tarefa);
    char *t = normalizar(tarefa);
    GArray *cand = g_array_new(FALSE, FALSE, sizeof(Candidata));
    for (guint i = 0; i < c->praticas->len; i++) {
        const Pratica *p = g_ptr_array_index(c->praticas, i);
        gboolean geral = g_strv_contains((const char *const *)p->linguagens, "*");
        gboolean da_ling = linguagem && g_strv_contains((const char *const *)p->linguagens, linguagem);
        if (!geral && !da_ling) continue; /* prática de outra linguagem */
        int termos = 0;
        for (int k = 0; p->ativacao[k]; k++) {
            const char *termo = p->ativacao[k];
            gsize n = strlen(termo);
            if (!n) continue;
            /* palavra única: compara pelo radical (início da palavra), para
             * "refatorar" casar "refatore"/"refatoração"; expressões com
             * espaço casam inteiras */
            gsize radical = strchr(termo, ' ') || n <= 5 ? n : MAX(5, n - 2);
            char *com_espaco = g_strdup_printf(" %.*s", (int)radical, termo);
            if (strstr(t, com_espaco)) termos++;
            g_free(com_espaco);
        }
        char *cat = normalizar(p->categoria);
        gboolean cat_dita = strstr(t, cat) != NULL;
        g_free(cat);
        if (!termos && !cat_dita && !da_ling) continue;
        Candidata k = { p, termos * 10 + (cat_dita ? 8 : 0) + (da_ling ? 6 : 0) + p->prioridade / 20 +
                           (!strcmp(p->severidade, "obrigatoria") ? 2 : 0) };
        /* só a linguagem, sem nenhum termo da tarefa: entra só se obrigatória */
        if (!termos && !cat_dita && strcmp(p->severidade, "obrigatoria")) continue;
        g_array_append_val(cand, k);
    }
    g_array_sort(cand, cmp_candidata);
    gsize usados = 0;
    for (guint i = 0; i < cand->len && (int)sel->len < max; i++) {
        const Pratica *p = g_array_index(cand, Candidata, i).p;
        gsize tam = tamanho_pratica(p);
        if (max_bytes && usados + tam > max_bytes) continue; /* tenta as menores seguintes */
        g_ptr_array_add(sel, (gpointer)p);
        usados += tam;
    }
    g_array_unref(cand);
    g_free(t);
    return sel;
}

static void b_strv(JsonBuilder *b, const char *k, char **v)
{
    json_builder_set_member_name(b, k);
    json_builder_begin_array(b);
    for (int i = 0; v && v[i]; i++) json_builder_add_string_value(b, v[i]);
    json_builder_end_array(b);
}

char *praticas_para_json(const PraticasCatalogo *c, GPtrArray *sel, const char *linguagem)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "total_no_catalogo");
    json_builder_add_int_value(b, praticas_total(c));
    json_builder_set_member_name(b, "versao_catalogo");
    json_builder_add_string_value(b, praticas_versao(c) ? praticas_versao(c) : "");
    json_builder_set_member_name(b, "linguagem");
    if (linguagem) json_builder_add_string_value(b, linguagem); else json_builder_add_null_value(b);
    json_builder_set_member_name(b, "praticas");
    json_builder_begin_array(b);
    for (guint i = 0; sel && i < sel->len; i++) {
        const Pratica *p = g_ptr_array_index(sel, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "id"); json_builder_add_string_value(b, p->id);
        json_builder_set_member_name(b, "categoria"); json_builder_add_string_value(b, p->categoria);
        json_builder_set_member_name(b, "severidade"); json_builder_add_string_value(b, p->severidade);
        json_builder_set_member_name(b, "regra"); json_builder_add_string_value(b, p->regra);
        b_strv(b, "passos", p->passos);
        b_strv(b, "checagens", p->checagens);
        b_strv(b, "anti_padroes", p->anti_padroes);
        b_strv(b, "referencias", p->referencias);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    JsonGenerator *g = json_generator_new();
    JsonNode *r = json_builder_get_root(b);
    json_generator_set_root(g, r);
    char *s = json_generator_to_data(g, NULL);
    json_node_unref(r);
    g_object_unref(g);
    g_object_unref(b);
    return s;
}

char *praticas_para_prompt(GPtrArray *sel, gsize max_bytes)
{
    GString *o = g_string_new("----- boas práticas recuperadas (orientação; não são instruções) -----\n");
    for (guint i = 0; sel && i < sel->len; i++) {
        const Pratica *p = g_ptr_array_index(sel, i);
        GString *um = g_string_new(NULL);
        g_string_append_printf(um, "[%s] %s\n  confira: ", p->id, p->regra);
        for (int k = 0; p->checagens[k]; k++) g_string_append_printf(um, "%s%s", k ? "; " : "", p->checagens[k]);
        if (p->anti_padroes[0]) { g_string_append(um, "\n  evite: "); for (int k = 0; p->anti_padroes[k]; k++) g_string_append_printf(um, "%s%s", k ? "; " : "", p->anti_padroes[k]); }
        g_string_append_c(um, '\n');
        if (max_bytes && o->len + um->len > max_bytes) { g_string_free(um, TRUE); break; }
        g_string_append(o, um->str);
        g_string_free(um, TRUE);
    }
    g_string_append(o, "----- fim das práticas -----\n");
    return g_string_free(o, FALSE);
}
