/* lia-sessiond — implementação de sessiond/sintese_web.h. Ver o cabeçalho. */
#include "sintese_web.h"
#include "apresentacao.h"
#include "../web/pesquisa.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PONTOS 7
#define MAX_MARCOS 10
#define MAX_TEXTO_ITEM 450
#define MAX_TEXTO_RESPOSTA 900

void fonte_web_liberar(gpointer dados)
{
    FonteWeb *f = dados;
    if (!f) return;
    g_free(f->titulo); g_free(f->url); g_free(f->data); g_free(f->trecho); g_free(f->erro);
    g_free(f);
}

/* ------------------------------------------------------- utilidades --- */

static const char *jstr(JsonObject *o, const char *k)
{
    if (!o || !json_object_has_member(o, k)) return NULL;
    JsonNode *n = json_object_get_member(o, k);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
    return json_node_get_string(n);
}

static JsonArray *jarr(JsonObject *o, const char *k)
{
    if (!o || !json_object_has_member(o, k)) return NULL;
    JsonNode *n = json_object_get_member(o, k);
    return JSON_NODE_TYPE(n) == JSON_NODE_ARRAY ? json_node_get_array(n) : NULL;
}

/* objeto de carga: {"saida": {...}} (executor real empacotado) ou a raiz */
static JsonObject *carga(JsonParser *p, const char *json)
{
    if (!json || !json_parser_load_from_data(p, json, -1, NULL)) return NULL;
    JsonNode *r = json_parser_get_root(p);
    if (!r || JSON_NODE_TYPE(r) != JSON_NODE_OBJECT) return NULL;
    JsonObject *o = json_node_get_object(r);
    if (json_object_has_member(o, "saida") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, "saida")))
        return json_object_get_object_member(o, "saida");
    return o;
}

/* texto vindo da web ou do modelo: sem caracteres de controle, espaços
 * colapsados, cortado em palavra */
static char *texto_limpo(const char *s, gsize max_chars)
{
    char *valido = g_utf8_make_valid(s ? s : "", -1);
    GString *o = g_string_new(NULL);
    for (const char *p = valido; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (ch < 0x20 || ch == 0x7f || g_unichar_isspace(ch)) {
            if (o->len && o->str[o->len - 1] != ' ') g_string_append_c(o, ' ');
        } else {
            g_string_append_unichar(o, ch);
        }
    }
    g_free(valido);
    g_strstrip(o->str);
    o->len = strlen(o->str);
    if (max_chars && (gsize)g_utf8_strlen(o->str, -1) > max_chars) {
        char *corte = g_utf8_substring(o->str, 0, (glong)max_chars);
        char *esp = strrchr(corte, ' ');
        if (esp && esp - corte > (glong)max_chars / 2) *esp = '\0';
        g_string_assign(o, corte);
        g_string_append(o, "…");
        g_free(corte);
    }
    return g_string_free(o, FALSE);
}

static char *host_de(const char *url)
{
    GUri *u = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
    char *h = g_strdup(u && g_uri_get_host(u) ? g_uri_get_host(u) : "fonte sem título");
    if (u) g_uri_unref(u);
    return h;
}

/* ----------------------------------------------------------- fontes --- */

GPtrArray *sintese_web_fontes(const char *const *dados_json, const char *pedido, int max_fontes)
{
    if (max_fontes <= 0 || max_fontes > SINTESE_WEB_MAX_FONTES) max_fontes = SINTESE_WEB_MAX_FONTES;
    GPtrArray *lidas = g_ptr_array_new_with_free_func(fonte_web_liberar);
    GPtrArray *resumos = g_ptr_array_new_with_free_func(fonte_web_liberar);
    for (int i = 0; dados_json && dados_json[i]; i++) {
        JsonParser *p = json_parser_new();
        JsonObject *d = carga(p, dados_json[i]);
        if (!d) { g_object_unref(p); continue; }
        JsonArray *res = jarr(d, "resultados");
        const char *conteudo = jstr(d, "conteudo");
        if (conteudo && jstr(d, "url")) {
            /* web.abrir: a página inteira; extrai o trecho da pergunta */
            FonteWeb *f = g_new0(FonteWeb, 1);
            f->url = g_strdup(jstr(d, "url"));
            f->titulo = jstr(d, "titulo") ? texto_limpo(jstr(d, "titulo"), 200) : host_de(f->url);
            f->data = g_strdup(jstr(d, "data"));
            char *trecho = lia_web_trecho_relevante(conteudo, pedido, SINTESE_WEB_MAX_TRECHO);
            f->trecho = g_utf8_make_valid(trecho, -1);
            g_free(trecho);
            f->lida = TRUE;
            g_ptr_array_add(*f->trecho ? lidas : resumos, f);
        } else if (res && !jarr(d, "cotacoes")) {
            for (guint k = 0; k < json_array_get_length(res); k++) {
                JsonNode *el = json_array_get_element(res, k);
                if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) continue;
                JsonObject *r = json_node_get_object(el);
                const char *url = jstr(r, "url");
                if (!url || !g_str_has_prefix(url, "https://")) continue;
                gboolean repetida = FALSE;
                for (guint j = 0; j < lidas->len; j++) if (!g_strcmp0(((FonteWeb *)g_ptr_array_index(lidas, j))->url, url)) repetida = TRUE;
                for (guint j = 0; j < resumos->len; j++) if (!g_strcmp0(((FonteWeb *)g_ptr_array_index(resumos, j))->url, url)) repetida = TRUE;
                if (repetida) continue;
                FonteWeb *f = g_new0(FonteWeb, 1);
                f->url = g_strdup(url);
                f->titulo = jstr(r, "titulo") && *jstr(r, "titulo") ? texto_limpo(jstr(r, "titulo"), 200) : host_de(url);
                f->data = g_strdup(jstr(r, "data"));
                f->erro = jstr(r, "erro_abertura") ? texto_limpo(jstr(r, "erro_abertura"), 160) : NULL;
                const char *c = jstr(r, "conteudo");
                gboolean aberta = json_object_has_member(r, "aberta") && JSON_NODE_HOLDS_VALUE(json_object_get_member(r, "aberta")) &&
                                  json_object_get_boolean_member(r, "aberta") && c && *c;
                f->lida = aberta;
                f->trecho = aberta ? g_utf8_make_valid(c, -1) : texto_limpo(jstr(r, "resumo"), 400);
                g_ptr_array_add(aberta ? lidas : resumos, f);
            }
        }
        g_object_unref(p);
    }
    GPtrArray *fontes = g_ptr_array_new_with_free_func(fonte_web_liberar);
    for (guint i = 0; i < lidas->len && (int)fontes->len < max_fontes; i++)
        g_ptr_array_add(fontes, g_ptr_array_steal_index(lidas, i--));
    /* com duas ou mais páginas lidas, resumos de buscador não entram: a
     * resposta se apoia só em texto de página (e só fontes realmente
     * usadas aparecem numeradas) */
    for (guint i = 0; fontes->len < 2 && i < resumos->len && (int)fontes->len < max_fontes; i++) {
        FonteWeb *f = g_ptr_array_index(resumos, i);
        if (!f->trecho || g_utf8_strlen(f->trecho, -1) < 40) continue; /* resumo vazio não sustenta nada */
        g_ptr_array_add(fontes, g_ptr_array_steal_index(resumos, i--));
    }
    g_ptr_array_unref(lidas);
    g_ptr_array_unref(resumos);
    return fontes;
}

char *sintese_web_secao_fontes(GPtrArray *fontes)
{
    GString *o = g_string_new("Fontes:\n");
    for (guint i = 0; i < fontes->len; i++) {
        FonteWeb *f = g_ptr_array_index(fontes, i);
        g_string_append_printf(o, "  [%u] %s\n      %s\n", i + 1, f->titulo, f->url);
        if (f->lida && f->data) g_string_append_printf(o, "      página lida agora; data declarada pela página: %s\n", f->data);
        else if (f->lida) g_string_append(o, "      página lida agora; a página não declara data\n");
        else if (f->erro) g_string_append_printf(o, "      só o resumo do buscador (a página não pôde ser aberta: %s)\n", f->erro);
        else g_string_append(o, "      só o resumo do buscador (página não aberta)\n");
    }
    if (fontes->len == 0) g_string_append(o, "  (nenhuma)\n");
    g_strchomp(o->str);
    o->len = strlen(o->str);
    return g_string_free(o, FALSE);
}

/* Para o prompt: cada avaliação de token de prompt custa caro numa CPU
 * (medido: ~32 tokens/s com llama3.2:3b), então cada fonte vai com até
 * SINTESE_WEB_MAX_TRECHO_PROMPT caracteres, cortados em fim de frase. As
 * fontes só com resumo do buscador entram apenas se houver menos de duas
 * páginas lidas. */
#define SINTESE_WEB_MAX_TRECHO_PROMPT 1000

char *sintese_web_fontes_para_prompt(GPtrArray *fontes)
{
    guint lidas = 0;
    for (guint i = 0; i < fontes->len; i++) if (((FonteWeb *)g_ptr_array_index(fontes, i))->lida) lidas++;
    GString *o = g_string_new(NULL);
    for (guint i = 0; i < fontes->len; i++) {
        FonteWeb *f = g_ptr_array_index(fontes, i);
        if (!f->lida && lidas >= 2) continue;
        char *trecho = g_strdup(f->trecho ? f->trecho : "");
        if (g_utf8_strlen(trecho, -1) > SINTESE_WEB_MAX_TRECHO_PROMPT) {
            char *corte = g_utf8_substring(trecho, 0, SINTESE_WEB_MAX_TRECHO_PROMPT);
            char *ponto = g_strrstr(corte, ". ");
            if (ponto && ponto - corte > SINTESE_WEB_MAX_TRECHO_PROMPT / 2) ponto[1] = '\0';
            g_free(trecho);
            trecho = corte;
        }
        /* sem a data da página: o modelo a confundia com data de evento
         * ("2016 — Elixir é mencionado na Wikipédia"); ela fica só na seção Fontes */
        g_string_append_printf(o, "[%u] %s\n%s\n\n", i + 1, f->titulo, trecho);
        g_free(trecho);
    }
    return g_string_free(o, FALSE);
}

/* ---------------------------------------------------------- cotação --- */

static char *valor_br(double v)
{
    char buf[64];
    g_ascii_formatd(buf, sizeof buf, "%.4f", v);
    for (char *p = buf; *p; p++) if (*p == '.') *p = ',';
    return g_strdup(buf);
}

char *sintese_web_cotacao(const char *const *dados_json)
{
    for (int i = 0; dados_json && dados_json[i]; i++) {
        JsonParser *p = json_parser_new();
        JsonObject *d = carga(p, dados_json[i]);
        JsonArray *cot = jarr(d, "cotacoes");
        if (!cot || json_array_get_length(cot) == 0) { g_object_unref(p); continue; }
        const char *fonte = jstr(d, "fonte") ? jstr(d, "fonte") : "fonte não informada";
        const char *url = NULL;
        JsonArray *res = jarr(d, "resultados");
        if (res && json_array_get_length(res) && JSON_NODE_HOLDS_OBJECT(json_array_get_element(res, 0)))
            url = jstr(json_array_get_object_element(res, 0), "url");
        GString *r = g_string_new("Resposta: ");
        GString *pontos = g_string_new(NULL);
        const char *data_ref = NULL;
        for (guint k = 0; k < json_array_get_length(cot); k++) {
            JsonNode *el = json_array_get_element(cot, k);
            if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) continue;
            JsonObject *c = json_node_get_object(el);
            JsonNode *nv = json_object_has_member(c, "valor") ? json_object_get_member(c, "valor") : NULL;
            if (!nv || JSON_NODE_TYPE(nv) != JSON_NODE_VALUE) continue;
            double v = json_node_get_value_type(nv) == G_TYPE_INT64 ? (double)json_node_get_int(nv) : json_node_get_double(nv);
            char *vs = valor_br(v);
            const char *base = jstr(c, "base") ? jstr(c, "base") : "?", *dest = jstr(c, "destino") ? jstr(c, "destino") : "?";
            const char *par = jstr(c, "par"), *data = jstr(c, "data");
            if (data) data_ref = data;
            g_string_append_printf(r, "%s1 %s = %s %s%s%s%s", k ? "; " : "", base, vs, dest,
                                   par ? " (" : "", par ? par : "", par ? ")" : "");
            g_string_append_printf(pontos, "  • 1 %s = %s %s — data de referência: %s [1]\n", base, vs, dest, data ? data : "não informada");
            g_free(vs);
        }
        g_string_append_printf(r, ", segundo %s%s%s.", fonte, data_ref ? ", com data de referência " : "", data_ref ? data_ref : "");
        GString *o = g_string_new(NULL);
        g_string_append_printf(o, "%s\n\nDetalhes:\n%s\nObservação: é a taxa de referência publicada pela fonte (não a cotação de "
                               "compra/venda de um banco ou casa de câmbio, que inclui spread e varia ao longo do dia).\n\n",
                               r->str, pontos->str);
        g_string_append_printf(o, "Fontes:\n  [1] %s\n      %s\n      dado estruturado da API; data de referência: %s",
                               fonte, url ? url : "(URL não informada)", data_ref ? data_ref : "não informada");
        g_string_free(r, TRUE);
        g_string_free(pontos, TRUE);
        g_object_unref(p);
        return g_string_free(o, FALSE);
    }
    return NULL;
}

/* ---------------------------------------------------------- síntese --- */

const char *sintese_web_esquema(void)
{
    return "{\"type\":\"object\",\"properties\":{"
           "\"resposta\":{\"type\":\"string\"},"
           "\"pontos\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
               "\"texto\":{\"type\":\"string\"},\"fontes\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}},"
               "\"required\":[\"texto\",\"fontes\"]}},"
           "\"linha_do_tempo\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
               "\"quando\":{\"type\":\"string\"},\"evento\":{\"type\":\"string\"},"
               "\"fontes\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}},"
               "\"required\":[\"quando\",\"evento\",\"fontes\"]}},"
           "\"lacunas\":{\"type\":\"string\"}},"
           "\"required\":[\"resposta\",\"pontos\",\"linha_do_tempo\",\"lacunas\"]}";
}

typedef struct {
    char *quando;   /* só na linha do tempo */
    char *texto;
    int fontes[SINTESE_WEB_MAX_FONTES];
    int n_fontes;
} Item;

static void item_liberar(gpointer d)
{
    Item *it = d;
    g_free(it->quando); g_free(it->texto); g_free(it);
}

/* tira marcas "[1]", "[1, 2]" que o modelo escreve no texto (as citações
 * são remontadas pelo daemon), acrescentando-as às fontes do item */
static char *tirar_marcas(const char *texto, Item *it, guint n_fontes)
{
    GRegex *r = g_regex_new("\\s*\\[(\\d+(?:\\s*,\\s*\\d+)*)\\]", 0, 0, NULL);
    GMatchInfo *mi = NULL;
    g_regex_match(r, texto, 0, &mi);
    while (g_match_info_matches(mi)) {
        char *g = g_match_info_fetch(mi, 1);
        char **ns = g_strsplit(g, ",", -1);
        for (int k = 0; ns[k]; k++) {
            int v = atoi(g_strstrip(ns[k]));
            gboolean ja = FALSE;
            for (int j = 0; j < it->n_fontes; j++) if (it->fontes[j] == v) ja = TRUE;
            if (!ja && v >= 1 && v <= (int)n_fontes && it->n_fontes < SINTESE_WEB_MAX_FONTES) it->fontes[it->n_fontes++] = v;
        }
        g_strfreev(ns); g_free(g);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    char *sem = g_regex_replace(r, texto, -1, 0, "", 0, NULL);
    g_regex_unref(r);
    return sem ? sem : g_strdup(texto);
}

/* evidências contra as quais conferir os números de um item: os trechos e
 * títulos das fontes citadas (ou de todas, se nenhuma), datas e o pedido */
static gboolean conferir(const char *texto, const Item *it, GPtrArray *fontes, const char *pedido, char **suspeito)
{
    GPtrArray *ev = g_ptr_array_new();
    for (guint i = 0; i < fontes->len; i++) {
        gboolean citada = it->n_fontes == 0;
        for (int k = 0; k < it->n_fontes; k++) if (it->fontes[k] == (int)i + 1) citada = TRUE;
        if (!citada) continue;
        FonteWeb *f = g_ptr_array_index(fontes, i);
        g_ptr_array_add(ev, f->trecho ? f->trecho : "");
        g_ptr_array_add(ev, f->titulo ? f->titulo : "");
    }
    g_ptr_array_add(ev, (gpointer)(pedido ? pedido : ""));
    g_ptr_array_add(ev, NULL);
    gboolean ok = apresentacao_resumo_fundamentado(texto, (const char *const *)ev->pdata, suspeito);
    g_ptr_array_free(ev, TRUE);
    return ok;
}

/* Suporte lexical de um item numa fonte: fração dos termos significativos
 * do item (radical de 5 letras) que aparecem no trecho da fonte. */
static double suporte_lexico(const char *texto, const FonteWeb *f)
{
    char **ti = lia_web_termos(texto);
    char **tf = lia_web_termos(f->trecho ? f->trecho : "");
    guint total = 0, achados = 0;
    for (guint i = 0; ti[i]; i++) {
        total++;
        gsize n = strlen(ti[i]);
        gsize r = n >= 6 ? 5 : n;
        for (guint k = 0; tf[k]; k++)
            if (!strncmp(tf[k], ti[i], r) && (r == 5 || strlen(tf[k]) == n)) { achados++; break; }
    }
    g_strfreev(ti);
    g_strfreev(tf);
    return total ? (double)achados / total : 0.0;
}

#define SUPORTE_MINIMO_CITADA 0.4
#define SUPORTE_MINIMO_ATRIBUIR 0.5

/* Garante que a citação do item aponta para fonte(s) que de fato tratam do
 * que ele diz: se nenhuma das citadas tem suporte lexical suficiente (ou o
 * modelo não citou nada), a citação passa a ser a fonte de maior suporte —
 * se essa tiver suporte mínimo. FALSE: nenhuma fonte sustenta o item. */
static gboolean garantir_suporte(const char *texto, Item *it, GPtrArray *fontes)
{
    double melhor_citada = 0.0;
    for (int k = 0; k < it->n_fontes; k++) {
        double sp = suporte_lexico(texto, g_ptr_array_index(fontes, (guint)it->fontes[k] - 1));
        if (sp > melhor_citada) melhor_citada = sp;
    }
    if (it->n_fontes > 0 && melhor_citada >= SUPORTE_MINIMO_CITADA) return TRUE;
    int melhor = -1;
    double melhor_sp = 0.0;
    for (guint i = 0; i < fontes->len; i++) {
        double sp = suporte_lexico(texto, g_ptr_array_index(fontes, i));
        if (sp > melhor_sp) { melhor_sp = sp; melhor = (int)i; }
    }
    if (melhor < 0 || melhor_sp < SUPORTE_MINIMO_ATRIBUIR) return FALSE;
    it->fontes[0] = melhor + 1;
    it->n_fontes = 1;
    return TRUE;
}

/* O modelo pequeno às vezes cita a fonte errada para um fato que ESTÁ
 * numa das outras. Em vez de descartar o item, a citação passa a ser a
 * fonte (uma só) cujo trecho contém todos os números do item. Se nenhuma
 * contiver, o item continua sem respaldo e é descartado. */
static void reatribuir_citacao(const char *texto, Item *it, GPtrArray *fontes, const char *pedido)
{
    for (guint i = 0; i < fontes->len; i++) {
        Item tentativa = { 0 };
        tentativa.fontes[0] = (int)i + 1;
        tentativa.n_fontes = 1;
        if (conferir(texto, &tentativa, fontes, pedido, NULL)) {
            it->fontes[0] = (int)i + 1;
            it->n_fontes = 1;
            return;
        }
    }
}

static void anexar_citacao(GString *o, const Item *it)
{
    if (it->n_fontes == 0) return;
    g_string_append(o, " [");
    for (int k = 0; k < it->n_fontes; k++) g_string_append_printf(o, "%s%d", k ? ", " : "", it->fontes[k]);
    g_string_append_c(o, ']');
}

static gboolean quase_igual(const char *a, const char *b)
{
    char **ta = lia_web_termos(a), **tb = lia_web_termos(b);
    guint na = g_strv_length(ta), nb = g_strv_length(tb), comuns = 0;
    for (guint i = 0; i < na; i++) if (g_strv_contains((const char *const *)tb, ta[i])) comuns++;
    g_strfreev(ta); g_strfreev(tb);
    guint menor = MIN(na, nb);
    return menor > 0 && comuns * 10 >= menor * 7;
}

static char *renderizar(const char *resposta, GPtrArray *pontos, GPtrArray *marcos, const char *lacunas,
                        GPtrArray *fontes, const char *nota)
{
    GString *o = g_string_new(NULL);
    g_string_append_printf(o, "Resposta: %s\n", resposta);
    if (pontos && pontos->len) {
        g_string_append(o, "\nPontos principais:\n");
        for (guint i = 0; i < pontos->len; i++) {
            Item *it = g_ptr_array_index(pontos, i);
            g_string_append_printf(o, "  • %s", it->texto);
            anexar_citacao(o, it);
            g_string_append_c(o, '\n');
        }
    }
    if (marcos && marcos->len) {
        g_string_append(o, "\nLinha do tempo:\n");
        for (guint i = 0; i < marcos->len; i++) {
            Item *it = g_ptr_array_index(marcos, i);
            g_string_append_printf(o, "  • %s — %s", it->quando, it->texto);
            anexar_citacao(o, it);
            g_string_append_c(o, '\n');
        }
    }
    if (lacunas && *lacunas)
        g_string_append_printf(o, "\nO que as fontes não esclarecem: %s\n", lacunas);
    char *fs = sintese_web_secao_fontes(fontes);
    g_string_append_printf(o, "\n%s", fs);
    g_free(fs);
    if (nota && *nota) g_string_append_printf(o, "\n\nComo esta resposta foi montada: %s", nota);
    return g_string_free(o, FALSE);
}

static int ler_fontes_item(JsonObject *o, Item *it, guint n_fontes)
{
    int invalidas = 0;
    JsonArray *fs = jarr(o, "fontes");
    for (guint k = 0; fs && k < json_array_get_length(fs); k++) {
        JsonNode *n = json_array_get_element(fs, k);
        if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE) { invalidas++; continue; }
        int v;
        if (json_node_get_value_type(n) == G_TYPE_INT64) v = (int)json_node_get_int(n);
        else if (json_node_get_value_type(n) == G_TYPE_STRING && g_ascii_isdigit(*json_node_get_string(n)))
            v = atoi(json_node_get_string(n)); /* "fontes": ["1"] */
        else { invalidas++; continue; }
        if (v < 1 || v > (int)n_fontes) { invalidas++; continue; }
        gboolean ja = FALSE;
        for (int j = 0; j < it->n_fontes; j++) if (it->fontes[j] == v) ja = TRUE;
        if (!ja && it->n_fontes < SINTESE_WEB_MAX_FONTES) it->fontes[it->n_fontes++] = v;
    }
    return invalidas;
}

/* A saída do modelo foi cortada pelo limite de tokens (JSON incompleto):
 * aproveita o que está COMPLETO — a "resposta" e cada objeto de ponto ou
 * marco fechado — e monta um JSON válido só com isso. Nada é completado
 * por palpite: um item cortado no meio é simplesmente perdido. */
static char *salvar_json_cortado(const char *texto)
{
    GRegex *r_resp = g_regex_new("\"resposta\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"", 0, 0, NULL);
    GRegex *r_ponto = g_regex_new("\\{\\s*\"texto\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"\\s*,\\s*\"fontes\"\\s*:\\s*\\[([\\d,\\s]*)\\]\\s*\\}", 0, 0, NULL);
    GRegex *r_marco = g_regex_new("\\{\\s*\"quando\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"\\s*,\\s*\"evento\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"\\s*,\\s*\"fontes\"\\s*:\\s*\\[([\\d,\\s]*)\\]\\s*\\}", 0, 0, NULL);
    GMatchInfo *mi = NULL;
    char *resposta = NULL;
    if (r_resp && g_regex_match(r_resp, texto, 0, &mi)) resposta = g_match_info_fetch(mi, 1);
    g_match_info_free(mi);
    GString *o = g_string_new("{\"resposta\":\"");
    g_string_append(o, resposta ? resposta : "");
    g_string_append(o, "\",\"pontos\":[");
    int n = 0;
    if (r_ponto && g_regex_match(r_ponto, texto, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            char *t = g_match_info_fetch(mi, 1), *f = g_match_info_fetch(mi, 2);
            g_string_append_printf(o, "%s{\"texto\":\"%s\",\"fontes\":[%s]}", n++ ? "," : "", t, f);
            g_free(t); g_free(f);
            g_match_info_next(mi, NULL);
        }
    }
    g_match_info_free(mi);
    g_string_append(o, "],\"linha_do_tempo\":[");
    int m = 0;
    if (r_marco && g_regex_match(r_marco, texto, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            char *q = g_match_info_fetch(mi, 1), *e = g_match_info_fetch(mi, 2), *f = g_match_info_fetch(mi, 3);
            g_string_append_printf(o, "%s{\"quando\":\"%s\",\"evento\":\"%s\",\"fontes\":[%s]}", m++ ? "," : "", q, e, f);
            g_free(q); g_free(e); g_free(f);
            g_match_info_next(mi, NULL);
        }
    }
    g_match_info_free(mi);
    g_string_append(o, "],\"lacunas\":\"\"}");
    if (r_resp) g_regex_unref(r_resp);
    if (r_ponto) g_regex_unref(r_ponto);
    if (r_marco) g_regex_unref(r_marco);
    gboolean util = (resposta && *resposta) || n > 0;
    g_free(resposta);
    if (!util) { g_string_free(o, TRUE); return NULL; }
    return g_string_free(o, FALSE);
}

char *sintese_web_renderizar(const char *json_modelo, GPtrArray *fontes, const char *pedido,
                             int *descartados, char **motivo)
{
    if (descartados) *descartados = 0;
    if (motivo) *motivo = NULL;
    JsonParser *p = json_parser_new();
    /* sem saída restrita por esquema o modelo pode cercar o JSON com texto
     * ou com ```json ... ```: usa do primeiro '{' ao último '}' */
    const char *bruto = json_modelo ? json_modelo : "";
    const char *abre = strchr(bruto, '{');
    const char *fecha = strrchr(bruto, '}');
    char *recorte = abre ? (fecha && fecha > abre ? g_strndup(abre, (gsize)(fecha - abre + 1)) : g_strdup(abre)) : g_strdup(bruto);
    const char *ini = recorte;
    gboolean cortado = FALSE;
    if (!json_parser_load_from_data(p, ini, -1, NULL) || !json_parser_get_root(p) ||
        JSON_NODE_TYPE(json_parser_get_root(p)) != JSON_NODE_OBJECT) {
        char *salvo = *ini == '{' ? salvar_json_cortado(ini) : NULL;
        if (!salvo || !json_parser_load_from_data(p, salvo, -1, NULL)) {
            if (motivo) *motivo = g_strdup("o modelo não devolveu o JSON pedido");
            g_free(salvo);
            g_free(recorte);
            g_object_unref(p);
            return NULL;
        }
        g_free(salvo);
        cortado = TRUE;
    }
    JsonObject *raiz = json_node_get_object(json_parser_get_root(p));
    GString *motivos = g_string_new(cortado ? "a saída do modelo foi cortada no limite de tokens; só os itens completos foram usados" : "");
    int n_desc = 0;
    GPtrArray *pontos = g_ptr_array_new_with_free_func(item_liberar);
    GPtrArray *marcos = g_ptr_array_new_with_free_func(item_liberar);

    JsonArray *ps = jarr(raiz, "pontos");
    for (guint i = 0; ps && i < json_array_get_length(ps) && pontos->len < MAX_PONTOS; i++) {
        JsonNode *el = json_array_get_element(ps, i);
        /* o modelo às vezes devolve o ponto como texto simples, sem fontes */
        gboolean so_texto = JSON_NODE_TYPE(el) == JSON_NODE_VALUE && json_node_get_value_type(el) == G_TYPE_STRING;
        if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT && !so_texto) { n_desc++; continue; }
        JsonObject *o = so_texto ? NULL : json_node_get_object(el);
        Item *it = g_new0(Item, 1);
        if (o) ler_fontes_item(o, it, fontes->len);
        const char *bruto_texto = so_texto ? json_node_get_string(el) : (jstr(o, "texto") ? jstr(o, "texto") : "");
        char *sem = tirar_marcas(bruto_texto, it, fontes->len);
        it->texto = texto_limpo(sem, MAX_TEXTO_ITEM);
        g_free(sem);
        char *suspeito = NULL;
        gboolean repetido = FALSE;
        for (guint k = 0; k < pontos->len; k++) if (quase_igual(((Item *)g_ptr_array_index(pontos, k))->texto, it->texto)) repetido = TRUE;
        if (g_utf8_strlen(it->texto, -1) < 12 || repetido) { item_liberar(it); continue; }
        if (!garantir_suporte(it->texto, it, fontes)) {
            n_desc++;
            if (motivos->len < 400) g_string_append_printf(motivos, "%sponto sem respaldo no texto das fontes", motivos->len ? "; " : "");
            item_liberar(it);
            continue;
        }
        if (!conferir(it->texto, it, fontes, pedido, NULL))
            reatribuir_citacao(it->texto, it, fontes, pedido);
        if (it->n_fontes == 0 || !conferir(it->texto, it, fontes, pedido, &suspeito)) {
            n_desc++;
            if (suspeito && motivos->len < 400) g_string_append_printf(motivos, "%sponto com \"%s\", que não está nas fontes citadas", motivos->len ? "; " : "", suspeito);
            else if (it->n_fontes == 0 && motivos->len < 400) g_string_append_printf(motivos, "%sponto sem fonte citada", motivos->len ? "; " : "");
            g_free(suspeito);
            item_liberar(it);
            continue;
        }
        g_ptr_array_add(pontos, it);
    }

    JsonArray *ls = jarr(raiz, "linha_do_tempo");
    for (guint i = 0; ls && i < json_array_get_length(ls) && marcos->len < MAX_MARCOS; i++) {
        JsonNode *el = json_array_get_element(ls, i);
        if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) { n_desc++; continue; }
        JsonObject *o = json_node_get_object(el);
        Item *it = g_new0(Item, 1);
        ler_fontes_item(o, it, fontes->len);
        char *sem = tirar_marcas(jstr(o, "evento") ? jstr(o, "evento") : "", it, fontes->len);
        it->texto = texto_limpo(sem, MAX_TEXTO_ITEM);
        it->quando = texto_limpo(jstr(o, "quando"), 40);
        g_free(sem);
        gboolean marco_repetido = FALSE;
        for (guint k = 0; k < marcos->len; k++) {
            Item *outro = g_ptr_array_index(marcos, k);
            if (!g_strcmp0(outro->quando, it->quando) || quase_igual(outro->texto, it->texto)) marco_repetido = TRUE;
        }
        if (marco_repetido) { item_liberar(it); continue; }
        char *junto = g_strdup_printf("%s %s", it->quando, it->texto);
        char *suspeito = NULL;
        gboolean sustentado = garantir_suporte(junto, it, fontes);
        if (sustentado && !conferir(junto, it, fontes, pedido, NULL))
            reatribuir_citacao(junto, it, fontes, pedido);
        if (!sustentado || !*it->quando || g_utf8_strlen(it->texto, -1) < 6 || it->n_fontes == 0 ||
            !conferir(junto, it, fontes, pedido, &suspeito)) {
            n_desc++;
            if (suspeito && motivos->len < 400) g_string_append_printf(motivos, "%smarco \"%s\" sem respaldo nas fontes", motivos->len ? "; " : "", suspeito);
            g_free(suspeito); g_free(junto);
            item_liberar(it);
            continue;
        }
        g_free(junto);
        g_ptr_array_add(marcos, it);
    }

    /* a conclusão: conferida contra TODAS as fontes */
    Item todas = { 0 };
    char *resp_bruta = tirar_marcas(jstr(raiz, "resposta") ? jstr(raiz, "resposta") : "", &todas, fontes->len);
    char *resposta = texto_limpo(resp_bruta, MAX_TEXTO_RESPOSTA);
    g_free(resp_bruta);
    Item sem_citacao = { 0 };
    char *suspeito = NULL;
    gboolean resposta_ok = g_utf8_strlen(resposta, -1) >= 20 && !apresentacao_resumo_e_recusa(resposta) &&
                           conferir(resposta, &sem_citacao, fontes, pedido, &suspeito);
    if (!resposta_ok) {
        g_string_append_printf(motivos, "%sa conclusão do modelo foi trocada (%s)", motivos->len ? "; " : "",
                               suspeito ? "citava um número que não está nas fontes" :
                               apresentacao_resumo_e_recusa(resposta) ? "negava ter os dados que as fontes trazem" : "vazia ou curta demais");
        g_free(resposta);
        resposta = NULL;
        if (pontos->len) {
            /* sem conclusão confiável, a conclusão vira o fato mais importante conferido */
            Item *primeiro = g_ptr_array_index(pontos, 0);
            GString *r = g_string_new(primeiro->texto);
            anexar_citacao(r, primeiro);
            resposta = g_string_free(r, FALSE);
            g_ptr_array_remove_index(pontos, 0);
        }
    }
    g_free(suspeito);

    for (guint k = 0; resposta && k < pontos->len; k++)
        if (quase_igual(resposta, ((Item *)g_ptr_array_index(pontos, k))->texto)) g_ptr_array_remove_index(pontos, k--);
    char *lacunas = texto_limpo(jstr(raiz, "lacunas"), 500);
    {
        static const char *const PREFIXOS[] = { "o que as fontes não dizem é ", "o que as fontes não dizem são ",
                                                "o que as fontes não informam é ", "as fontes não dizem ", "as fontes não informam ", NULL };
        char *min = g_utf8_strdown(lacunas, -1);
        for (int i = 0; PREFIXOS[i]; i++) {
            if (!g_str_has_prefix(min, PREFIXOS[i])) continue;
            char *resto = g_strdup(lacunas + strlen(PREFIXOS[i]));
            if (*resto) { gunichar c0 = g_utf8_get_char(resto); char up[8] = { 0 }; g_unichar_to_utf8(g_unichar_toupper(c0), up);
                          char *novo = g_strconcat(up, g_utf8_next_char(resto), NULL); g_free(resto); resto = novo; }
            g_free(lacunas);
            lacunas = resto;
            break;
        }
        g_free(min);
        char *m2 = g_utf8_strdown(lacunas, -1);
        /* eco do formato pedido no prompt, não uma lacuna de verdade */
        if (strstr(m2, "fato curto") || strstr(m2, "frases com a conclusão") || strstr(m2, "o que as fontes não dizem") ||
            strstr(m2, "conclusão direta")) { g_free(lacunas); lacunas = g_strdup(""); }
        else if (!strcmp(m2, "nenhuma") || !strcmp(m2, "nada") || !strcmp(m2, "nenhuma.") || g_str_has_prefix(m2, "nenhuma lacuna")) { g_free(lacunas); lacunas = g_strdup(""); }
        g_free(m2);
    }
    char *saida = NULL;
    if (resposta) {
        GString *nota = g_string_new("síntese do modelo local a partir dos trechos lidos nas fontes acima; "
                                     "cada item cita as fontes que o sustentam, e todo número e data foi conferido contra elas");
        if (n_desc) g_string_append_printf(nota, " (%d item(ns) descartado(s) por não conferir)", n_desc);
        saida = renderizar(resposta, pontos, marcos, lacunas, fontes, nota->str);
        g_string_free(nota, TRUE);
    } else {
        g_string_append_printf(motivos, "%snenhum item da síntese pôde ser conferido", motivos->len ? "; " : "");
    }
    if (descartados) *descartados = n_desc;
    if (motivo && motivos->len) *motivo = g_strdup(motivos->str);
    g_string_free(motivos, TRUE);
    g_free(resposta);
    g_free(lacunas);
    g_ptr_array_unref(pontos);
    g_ptr_array_unref(marcos);
    g_object_unref(p);
    g_free(recorte);
    return saida;
}

/* ------------------------------------------------------- extrativa --- */

typedef struct {
    char *frase;
    int fonte;      /* 1..n */
    int pontos;
    int ordem;      /* posição na fonte */
    int ano;        /* primeiro ano citado, ou 0 */
} Frase;

static void frase_liberar(gpointer d) { Frase *f = d; g_free(f->frase); g_free(f); }

static gint por_pontos(gconstpointer a, gconstpointer b)
{
    const Frase *x = *(Frase * const *)a, *y = *(Frase * const *)b;
    if (y->pontos != x->pontos) return y->pontos - x->pontos;
    if (x->fonte != y->fonte) return x->fonte - y->fonte;
    return x->ordem - y->ordem;
}

static gint por_ano(gconstpointer a, gconstpointer b)
{
    const Item *x = *(Item * const *)a, *y = *(Item * const *)b;
    return atoi(x->quando) - atoi(y->quando);
}

static gboolean contem_termo(const char *normalizada_frase, char **termos, int *n)
{
    *n = 0;
    for (int i = 0; termos[i]; i++) {
        char radical[8];
        gsize len = strlen(termos[i]);
        if (len >= 6) { memcpy(radical, termos[i], 5); radical[5] = '\0'; }
        else g_snprintf(radical, sizeof radical, "%s", termos[i]);
        if (strstr(normalizada_frase, radical)) (*n)++;
    }
    return *n > 0;
}

static gboolean pede_cronologia(const char *pedido)
{
    char *t = g_utf8_strdown(pedido ? pedido : "", -1);
    static const char *const S[] = { "histór", "histor", "evolu", "cronolog", "linha do tempo", "quando", "surg", "origem",
                                     "versões", "versoes", "versão", "lançament", "lancament", "trajet", NULL };
    gboolean r = FALSE;
    for (int i = 0; S[i] && !r; i++) r = strstr(t, S[i]) != NULL;
    g_free(t);
    return r;
}

char *sintese_web_extrativa(GPtrArray *fontes, const char *pedido, const char *nota)
{
    char **termos = lia_web_termos(pedido);
    GPtrArray *frases = g_ptr_array_new_with_free_func(frase_liberar);
    GRegex *ano = g_regex_new("\\b(1[89]\\d\\d|20\\d\\d)\\b", 0, 0, NULL);
    for (guint i = 0; i < fontes->len; i++) {
        FonteWeb *f = g_ptr_array_index(fontes, i);
        if (!f->trecho) continue;
        /* frases: termina em ". ", "! ", "? " ou fim de parágrafo */
        char *t = g_strdup(f->trecho);
        int ordem = 0;
        for (char *ini = t; *ini;) {
            char *p = ini;
            while (*p && *p != '\n' && !((*p == '.' || *p == '!' || *p == '?') && (p[1] == ' ' || p[1] == '\n' || !p[1]) &&
                                          !(p > ini && g_ascii_isdigit(p[-1]) && g_ascii_isdigit(p[1])))) p++;
            char *fim = *p && *p != '\n' ? p + 1 : p;
            char *frase = g_strndup(ini, (gsize)(fim - ini));
            g_strstrip(frase);
            ini = *fim ? fim + 1 : fim;
            glong n = g_utf8_strlen(frase, -1);
            if (n < 50 || n > 420 || !strchr(frase, ' ')) { g_free(frase); continue; }
            char *norm = g_utf8_strdown(frase, -1);
            char *sem_acento = g_str_to_ascii(norm, NULL);
            g_free(norm);
            int hits = 0;
            contem_termo(sem_acento, termos, &hits);
            g_free(sem_acento);
            Frase *fr = g_new0(Frase, 1);
            fr->frase = frase;
            fr->fonte = (int)i + 1;
            fr->ordem = ordem++;
            /* termos da pergunta, mas também posição: o começo de cada fonte
             * costuma trazer a definição/conclusão; frases enormes, não */
            fr->pontos = hits * 10 + (fr->ordem == 0 ? 12 : 0) - MIN(fr->ordem, 8) * 2 + (i == 0 ? 4 : 0) +
                         (f->lida ? 2 : 0) - (n > 280 ? 8 : 0);
            GMatchInfo *mi = NULL;
            if (g_regex_match(ano, frase, 0, &mi)) { char *a = g_match_info_fetch(mi, 1); fr->ano = atoi(a); g_free(a); }
            g_match_info_free(mi);
            g_ptr_array_add(frases, fr);
        }
        g_free(t);
    }
    g_regex_unref(ano);
    g_ptr_array_sort(frases, por_pontos);

    GPtrArray *escolhidas = g_ptr_array_new();
    for (guint i = 0; i < frases->len && escolhidas->len < 5; i++) {
        Frase *fr = g_ptr_array_index(frases, i);
        gboolean repetida = FALSE;
        for (guint k = 0; k < escolhidas->len; k++) if (quase_igual(((Frase *)g_ptr_array_index(escolhidas, k))->frase, fr->frase)) repetida = TRUE;
        if (!repetida) g_ptr_array_add(escolhidas, fr);
    }
    char *saida;
    if (escolhidas->len == 0) {
        g_ptr_array_free(escolhidas, TRUE);
        g_ptr_array_unref(frases);
        g_strfreev(termos);
        return sintese_web_sem_dados(fontes, pedido, NULL);
    }
    Frase *principal = g_ptr_array_index(escolhidas, 0);
    GString *resposta = g_string_new(principal->frase);
    g_string_append_printf(resposta, " [%d]", principal->fonte);
    GPtrArray *pontos = g_ptr_array_new_with_free_func(item_liberar);
    for (guint i = 1; i < escolhidas->len; i++) {
        Frase *fr = g_ptr_array_index(escolhidas, i);
        Item *it = g_new0(Item, 1);
        it->texto = g_strdup(fr->frase);
        it->fontes[it->n_fontes++] = fr->fonte;
        g_ptr_array_add(pontos, it);
    }
    GPtrArray *marcos = g_ptr_array_new_with_free_func(item_liberar);
    if (pede_cronologia(pedido)) {
        for (guint i = 0; i < frases->len && marcos->len < 6; i++) {
            Frase *fr = g_ptr_array_index(frases, i);
            if (!fr->ano) continue;
            gboolean mesmo_ano = FALSE;
            for (guint k = 0; k < marcos->len; k++) if (atoi(((Item *)g_ptr_array_index(marcos, k))->quando) == fr->ano) mesmo_ano = TRUE;
            if (mesmo_ano) continue;
            Item *it = g_new0(Item, 1);
            it->quando = g_strdup_printf("%d", fr->ano);
            it->texto = g_strdup(fr->frase);
            it->fontes[it->n_fontes++] = fr->fonte;
            g_ptr_array_add(marcos, it);
        }
        g_ptr_array_sort(marcos, por_ano);
    }
    GString *n = g_string_new("frases copiadas dos trechos lidos nas fontes, escolhidas por relevância para a pergunta (sem texto do modelo)");
    if (nota && *nota) g_string_append_printf(n, "; %s", nota);
    saida = renderizar(resposta->str, pontos, marcos, NULL, fontes, n->str);
    g_string_free(n, TRUE);
    g_string_free(resposta, TRUE);
    g_ptr_array_unref(pontos);
    g_ptr_array_unref(marcos);
    g_ptr_array_free(escolhidas, TRUE);
    g_ptr_array_unref(frases);
    g_strfreev(termos);
    return saida;
}

char *sintese_web_sem_dados(GPtrArray *fontes, const char *pedido, const char *erro)
{
    (void)pedido;
    GString *o = g_string_new(NULL);
    if (erro && *erro)
        g_string_append_printf(o, "Resposta: não consegui obter dados da web para responder — %s. Nada foi inventado: "
                               "sem fontes lidas, não há resposta verificável.", erro);
    else
        g_string_append(o, "Resposta: a pesquisa não encontrou fontes com conteúdo suficiente para responder com segurança. "
                           "Nada foi inventado.");
    g_string_append(o, "\n\nPróximo passo: reformular a pergunta com termos mais específicos (nome, versão, ano) ou indicar uma URL HTTPS para eu abrir.");
    if (fontes && fontes->len) {
        char *fs = sintese_web_secao_fontes(fontes);
        g_string_append_printf(o, "\n\n%s", fs);
        g_free(fs);
    }
    return g_string_free(o, FALSE);
}
