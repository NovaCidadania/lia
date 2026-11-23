/* lia-sessiond — implementação de sessiond/agente.h. Ver o cabeçalho. */
#include "agente.h"
#include "roteador.h"
#include "intencao_arquivos.h"
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------- utilidades --- */

const char *agente_passo_estado_texto(PassoEstado e)
{
    switch (e) {
    case PASSO_AGUARDANDO: return "aguardando";
    case PASSO_EXECUTANDO: return "executando";
    case PASSO_SUCESSO: return "sucesso";
    case PASSO_FALHA: return "falha";
    case PASSO_CANCELADO: return "cancelado";
    case PASSO_PULADO: return "pulado";
    }
    return "?";
}

const char *agente_compat_texto(AgenteCompat c)
{
    switch (c) {
    case COMPAT_COMPATIVEL: return "compativel";
    case COMPAT_PARCIAL: return "parcial";
    case COMPAT_INEXISTENTE: return "inexistente";
    case COMPAT_ESCLARECER: return "esclarecer";
    }
    return "?";
}

const char *agente_veredito_texto(AgenteVeredito v)
{
    switch (v) {
    case VEREDITO_SUCESSO: return "sucesso";
    case VEREDITO_INCOMPLETO: return "incompleto";
    case VEREDITO_REPLANEJAR: return "replanejar";
    case VEREDITO_FALHA: return "falha";
    }
    return "?";
}

static char **strv_de(const char *const *v)
{
    GPtrArray *a = g_ptr_array_new();
    for (int i = 0; v && v[i]; i++)
        g_ptr_array_add(a, g_strdup(v[i]));
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

static gboolean strv_tem(char **v, const char *s)
{
    return v && s && g_strv_contains((const char *const *)v, s);
}

static char **strv_acrescentar(char **v, const char *s)
{
    if (strv_tem(v, s)) return v;
    guint n = v ? g_strv_length(v) : 0;
    v = g_realloc(v, sizeof(char *) * (n + 2));
    v[n] = g_strdup(s);
    v[n + 1] = NULL;
    return v;
}

static void passo_liberar(gpointer dados)
{
    AgentePasso *s = dados;
    g_free(s->id);
    g_free(s->ferramenta);
    g_free(s->parametros_json);
    g_strfreev(s->depende_de);
    g_free(s->objetivo);
    g_strfreev(s->evidencia_esperada);
    g_free(s->dados_json);
    g_free(s->apresentacao);
    g_strfreev(s->evidencias);
    g_strfreev(s->limitacoes);
    g_free(s->erro);
    g_free(s);
}

AgentePlano *agente_plano_novo(const char *objetivo, const char *intencao)
{
    AgentePlano *p = g_new0(AgentePlano, 1);
    p->objetivo = g_strdup(objetivo ? objetivo : "");
    p->intencao = g_strdup(intencao ? intencao : "plano");
    p->passos = g_ptr_array_new_with_free_func(passo_liberar);
    p->orcamento_passos = AGENTE_MAX_PASSOS;
    p->orcamento_tempo_ms = AGENTE_ORCAMENTO_TEMPO_MS;
    p->versao = 1;
    p->compatibilidade = COMPAT_COMPATIVEL;
    return p;
}

void agente_plano_liberar(AgentePlano *p)
{
    if (!p) return;
    g_free(p->plano_id);
    g_free(p->objetivo);
    g_free(p->intencao);
    g_ptr_array_free(p->passos, TRUE);
    g_strfreev(p->criterios_sucesso);
    g_strfreev(p->capacidades);
    g_free(p->risco);
    g_free(p->hash);
    g_strfreev(p->limitacoes);
    g_free(p);
}

AgentePasso *agente_plano_acrescentar(AgentePlano *p, const char *id, const char *ferramenta,
                                       const char *parametros_json, const char *const *depende_de,
                                       const char *objetivo, const char *const *evidencias,
                                       int timeout_ms, gboolean permitir_falha)
{
    AgentePasso *s = g_new0(AgentePasso, 1);
    s->id = id && *id ? g_strdup(id) : g_strdup_printf("passo-%u", p->passos->len + 1);
    s->ferramenta = g_strdup(ferramenta ? ferramenta : "");
    s->parametros_json = g_strdup(parametros_json && *parametros_json ? parametros_json : "{}");
    s->depende_de = strv_de(depende_de);
    s->objetivo = g_strdup(objetivo ? objetivo : "");
    s->evidencia_esperada = strv_de(evidencias);
    s->timeout_ms = timeout_ms;
    s->permitir_falha = permitir_falha;
    g_ptr_array_add(p->passos, s);
    return s;
}

static AgentePasso *passo_por_id(const AgentePlano *p, const char *id)
{
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        if (strcmp(s->id, id) == 0) return s;
    }
    return NULL;
}

/* ordem dos riscos (docs/manifesto.md §1) */
static int nivel_risco(const char *r)
{
    static const char *const N[] = { "nenhum", "baixo", "medio", "alto", "admin", NULL };
    for (int i = 0; N[i]; i++)
        if (g_strcmp0(r, N[i]) == 0) return i;
    return 4; /* desconhecido: trata como o mais alto */
}

static void recalcular_hash(AgentePlano *p)
{
    GString *c = g_string_new(NULL);
    g_string_append_printf(c, "v%d|%s|", p->versao, p->objetivo);
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        char *deps = g_strjoinv(",", s->depende_de);
        g_string_append_printf(c, "%s|%s|%s|%s|%d|%d\n", s->id, s->ferramenta, s->parametros_json, deps,
                               s->timeout_ms, s->permitir_falha ? 1 : 0);
        g_free(deps);
    }
    g_free(p->hash);
    p->hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, c->str, (gssize)c->len);
    g_string_free(c, TRUE);
}

/* normaliza um objeto JSON em texto (forma compacta do json-glib); NULL se não for objeto */
static char *normalizar_objeto(const char *json)
{
    JsonParser *p = json_parser_new();
    char *saida = NULL;
    if (json_parser_load_from_data(p, json ? json : "{}", -1, NULL) && json_parser_get_root(p) &&
        JSON_NODE_TYPE(json_parser_get_root(p)) == JSON_NODE_OBJECT) {
        JsonGenerator *g = json_generator_new();
        json_generator_set_root(g, json_parser_get_root(p));
        saida = json_generator_to_data(g, NULL);
        g_object_unref(g);
    }
    g_object_unref(p);
    return saida;
}

char *agente_plano_validar(AgentePlano *p, const AgenteManifesto *mf)
{
    guint n = p->passos->len;
    if (n == 0)
        return g_strdup("plano_invalido: o plano não tem nenhum passo");
    if ((int)n > AGENTE_MAX_PASSOS)
        return g_strdup_printf("plano_invalido: %u passos, acima do limite de %d", n, AGENTE_MAX_PASSOS);
    if (p->orcamento_passos <= 0 || p->orcamento_passos > AGENTE_MAX_PASSOS)
        p->orcamento_passos = AGENTE_MAX_PASSOS;
    if (p->orcamento_tempo_ms <= 0 || p->orcamento_tempo_ms > AGENTE_ORCAMENTO_TEMPO_MS)
        p->orcamento_tempo_ms = AGENTE_ORCAMENTO_TEMPO_MS;

    int risco = 0;
    gboolean requer = FALSE, leitura = TRUE;
    char **caps = NULL;
    for (guint i = 0; i < n; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        if (!*s->id || strlen(s->id) > 64) {
            g_strfreev(caps);
            return g_strdup_printf("plano_invalido: id de passo vazio ou longo demais (passo %u)", i + 1);
        }
        for (guint j = 0; j < i; j++)
            if (strcmp(((AgentePasso *)g_ptr_array_index(p->passos, j))->id, s->id) == 0) {
                g_strfreev(caps);
                return g_strdup_printf("plano_invalido: id de passo repetido: '%s'", s->id);
            }
        AgenteFerramentaInfo info = { 0 };
        if (!*s->ferramenta || !mf->info(s->ferramenta, &info, mf->dados)) {
            g_strfreev(caps);
            return g_strdup_printf("ferramenta_desconhecida: %s (passo '%s')", *s->ferramenta ? s->ferramenta : "(vazia)", s->id);
        }
        if (!info.capacidade_permitida) {
            g_strfreev(caps);
            return g_strdup_printf("capacidade_negada: a ferramenta '%s' (passo '%s') pede capacidade que este executor não libera", s->ferramenta, s->id);
        }
        char *norm = normalizar_objeto(s->parametros_json);
        if (!norm) {
            g_strfreev(caps);
            return g_strdup_printf("parametro_invalido: parâmetros do passo '%s' não são um objeto JSON", s->id);
        }
        g_free(s->parametros_json);
        s->parametros_json = norm;
        char *erro = mf->validar(s->ferramenta, s->parametros_json, mf->dados);
        if (erro) {
            char *m = g_strdup_printf("parametro_invalido: passo '%s' (%s): %s", s->id, s->ferramenta, erro);
            g_free(erro);
            g_strfreev(caps);
            return m;
        }
        if (s->timeout_ms <= 0 || s->timeout_ms > AGENTE_ORCAMENTO_TEMPO_MS)
            s->timeout_ms = info.timeout_padrao_ms > 0 ? info.timeout_padrao_ms : 10000;
        s->somente_leitura = info.somente_leitura && !info.requer_aprovacao && nivel_risco(info.risco) == 0;
        s->requer_aprovacao = info.requer_aprovacao;
        if (nivel_risco(info.risco) > risco) risco = nivel_risco(info.risco);
        requer = requer || info.requer_aprovacao;
        leitura = leitura && s->somente_leitura;
        for (int k = 0; info.capacidades && info.capacidades[k]; k++)
            caps = strv_acrescentar(caps, info.capacidades[k]);
        for (int k = 0; s->depende_de[k]; k++) {
            if (strcmp(s->depende_de[k], s->id) == 0) {
                g_strfreev(caps);
                return g_strdup_printf("plano_invalido: o passo '%s' depende de si mesmo", s->id);
            }
            if (!passo_por_id(p, s->depende_de[k])) {
                g_strfreev(caps);
                return g_strdup_printf("plano_invalido: o passo '%s' depende de '%s', que não existe no plano", s->id, s->depende_de[k]);
            }
        }
    }
    /* ciclo de dependências: ordenação topológica (Kahn) precisa consumir todos */
    gboolean *feito = g_new0(gboolean, n);
    guint resolvidos = 0;
    for (gboolean andou = TRUE; andou;) {
        andou = FALSE;
        for (guint i = 0; i < n; i++) {
            if (feito[i]) continue;
            AgentePasso *s = g_ptr_array_index(p->passos, i);
            gboolean livre = TRUE;
            for (int k = 0; s->depende_de[k] && livre; k++)
                for (guint j = 0; j < n; j++)
                    if (!feito[j] && strcmp(((AgentePasso *)g_ptr_array_index(p->passos, j))->id, s->depende_de[k]) == 0)
                        livre = FALSE;
            if (livre) { feito[i] = TRUE; resolvidos++; andou = TRUE; }
        }
    }
    g_free(feito);
    if (resolvidos != n) {
        g_strfreev(caps);
        return g_strdup("plano_invalido: dependência circular entre os passos");
    }

    static const char *const NOMES[] = { "nenhum", "baixo", "medio", "alto", "admin" };
    g_free(p->risco);
    p->risco = g_strdup(NOMES[risco]);
    p->requer_aprovacao = requer;
    p->somente_leitura = leitura;
    g_strfreev(p->capacidades);
    p->capacidades = caps ? caps : g_new0(char *, 1);
    if (!p->criterios_sucesso || !p->criterios_sucesso[0]) {
        /* sem critérios explícitos: o que cada passo obrigatório declarou
         * esperar, ou — se não declarou nada — o próprio resultado dele */
        char **crit = NULL;
        for (guint i = 0; i < n; i++) {
            AgentePasso *s = g_ptr_array_index(p->passos, i);
            if (s->permitir_falha) continue;
            if (s->evidencia_esperada[0])
                for (int k = 0; s->evidencia_esperada[k]; k++)
                    crit = strv_acrescentar(crit, s->evidencia_esperada[k]);
            else {
                char *e = g_strdup_printf("resultado:%s", s->ferramenta);
                crit = strv_acrescentar(crit, e);
                g_free(e);
            }
        }
        g_strfreev(p->criterios_sucesso);
        p->criterios_sucesso = crit ? crit : g_new0(char *, 1);
    }
    if (!p->plano_id) {
        static guint contador;
        p->plano_id = g_strdup_printf("tarefa-%" G_GINT64_FORMAT "-%u", g_get_monotonic_time() / 1000, ++contador);
    }
    recalcular_hash(p);
    return NULL;
}

/* ------------------------------------------------------- plano <-> JSON --- */

static const char *jstr(JsonObject *o, const char *k)
{
    if (!o || !json_object_has_member(o, k)) return NULL;
    JsonNode *n = json_object_get_member(o, k);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
    return json_node_get_string(n);
}

static gboolean jint(JsonObject *o, const char *k, gint64 *v)
{
    if (!o || !json_object_has_member(o, k)) return FALSE;
    JsonNode *n = json_object_get_member(o, k);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_INT64) return FALSE;
    *v = json_node_get_int(n);
    return TRUE;
}

static JsonObject *jobj(JsonObject *o, const char *k)
{
    if (!o || !json_object_has_member(o, k)) return NULL;
    JsonNode *n = json_object_get_member(o, k);
    return JSON_NODE_TYPE(n) == JSON_NODE_OBJECT ? json_node_get_object(n) : NULL;
}

static JsonArray *jarr(JsonObject *o, const char *k)
{
    if (!o || !json_object_has_member(o, k)) return NULL;
    JsonNode *n = json_object_get_member(o, k);
    return JSON_NODE_TYPE(n) == JSON_NODE_ARRAY ? json_node_get_array(n) : NULL;
}

static char **jstrv(JsonObject *o, const char *k)
{
    GPtrArray *a = g_ptr_array_new();
    JsonArray *arr = jarr(o, k);
    for (guint i = 0; arr && i < json_array_get_length(arr) && i < 32; i++) {
        JsonNode *n = json_array_get_element(arr, i);
        if (JSON_NODE_TYPE(n) == JSON_NODE_VALUE && json_node_get_value_type(n) == G_TYPE_STRING)
            g_ptr_array_add(a, g_strdup(json_node_get_string(n)));
    }
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

AgentePlano *agente_plano_de_json(const char *json, const char *intencao, char **erro)
{
    if (erro) *erro = NULL;
    JsonParser *jp = json_parser_new();
    if (!json || !json_parser_load_from_data(jp, json, -1, NULL) || !json_parser_get_root(jp) ||
        JSON_NODE_TYPE(json_parser_get_root(jp)) != JSON_NODE_OBJECT) {
        if (erro) *erro = g_strdup("plano_invalido: o plano não é um objeto JSON");
        g_object_unref(jp);
        return NULL;
    }
    JsonObject *raiz = json_node_get_object(json_parser_get_root(jp));
    JsonArray *passos = jarr(raiz, "passos");
    if (!passos) {
        if (erro) *erro = g_strdup("plano_invalido: falta a lista 'passos'");
        g_object_unref(jp);
        return NULL;
    }
    if (json_array_get_length(passos) > AGENTE_MAX_PASSOS) {
        if (erro) *erro = g_strdup_printf("plano_invalido: %u passos, acima do limite de %d", json_array_get_length(passos), AGENTE_MAX_PASSOS);
        g_object_unref(jp);
        return NULL;
    }
    AgentePlano *p = agente_plano_novo(jstr(raiz, "objetivo"), intencao);
    p->criterios_sucesso = jstrv(raiz, "criterios_sucesso");
    for (guint i = 0; i < json_array_get_length(passos); i++) {
        JsonNode *n = json_array_get_element(passos, i);
        if (JSON_NODE_TYPE(n) != JSON_NODE_OBJECT) {
            if (erro) *erro = g_strdup_printf("plano_invalido: o passo %u não é um objeto", i + 1);
            agente_plano_liberar(p);
            g_object_unref(jp);
            return NULL;
        }
        JsonObject *o = json_node_get_object(n);
        char *params = NULL;
        if (json_object_has_member(o, "parametros")) {
            JsonGenerator *g = json_generator_new();
            json_generator_set_root(g, json_object_get_member(o, "parametros"));
            params = json_generator_to_data(g, NULL);
            g_object_unref(g);
        }
        char **deps = jstrv(o, "depende_de"), **evs = jstrv(o, "evidencia_esperada");
        gint64 timeout = 0;
        jint(o, "timeout_ms", &timeout);
        gboolean permitir = json_object_has_member(o, "permitir_falha") &&
                            JSON_NODE_TYPE(json_object_get_member(o, "permitir_falha")) == JSON_NODE_VALUE &&
                            json_node_get_value_type(json_object_get_member(o, "permitir_falha")) == G_TYPE_BOOLEAN &&
                            json_object_get_boolean_member(o, "permitir_falha");
        agente_plano_acrescentar(p, jstr(o, "id"), jstr(o, "ferramenta"), params, (const char *const *)deps,
                                 jstr(o, "objetivo"), (const char *const *)evs,
                                 (int)CLAMP(timeout, 0, AGENTE_ORCAMENTO_TEMPO_MS), permitir);
        g_free(params);
        g_strfreev(deps);
        g_strfreev(evs);
    }
    g_object_unref(jp);
    return p;
}

static void b_strv(JsonBuilder *b, const char *nome, char **v)
{
    json_builder_set_member_name(b, nome);
    json_builder_begin_array(b);
    for (int i = 0; v && v[i]; i++)
        json_builder_add_string_value(b, v[i]);
    json_builder_end_array(b);
}

static void b_bruto(JsonBuilder *b, const char *nome, const char *json)
{
    json_builder_set_member_name(b, nome);
    JsonParser *jp = json_parser_new();
    if (json && json_parser_load_from_data(jp, json, -1, NULL) && json_parser_get_root(jp))
        json_builder_add_value(b, json_node_copy(json_parser_get_root(jp)));
    else {
        json_builder_begin_object(b);
        json_builder_end_object(b);
    }
    g_object_unref(jp);
}

char *agente_plano_para_json(const AgentePlano *p, gboolean com_estado)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "plano_id"); json_builder_add_string_value(b, p->plano_id ? p->plano_id : "");
    json_builder_set_member_name(b, "hash"); json_builder_add_string_value(b, p->hash ? p->hash : "");
    json_builder_set_member_name(b, "versao"); json_builder_add_int_value(b, p->versao);
    json_builder_set_member_name(b, "objetivo"); json_builder_add_string_value(b, p->objetivo);
    json_builder_set_member_name(b, "intencao"); json_builder_add_string_value(b, p->intencao);
    json_builder_set_member_name(b, "compatibilidade"); json_builder_add_string_value(b, agente_compat_texto(p->compatibilidade));
    json_builder_set_member_name(b, "passos");
    json_builder_begin_array(b);
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "id"); json_builder_add_string_value(b, s->id);
        json_builder_set_member_name(b, "ferramenta"); json_builder_add_string_value(b, s->ferramenta);
        b_bruto(b, "parametros", s->parametros_json);
        b_strv(b, "depende_de", s->depende_de);
        json_builder_set_member_name(b, "objetivo"); json_builder_add_string_value(b, s->objetivo);
        b_strv(b, "evidencia_esperada", s->evidencia_esperada);
        json_builder_set_member_name(b, "timeout_ms"); json_builder_add_int_value(b, s->timeout_ms);
        json_builder_set_member_name(b, "permitir_falha"); json_builder_add_boolean_value(b, s->permitir_falha);
        json_builder_set_member_name(b, "somente_leitura"); json_builder_add_boolean_value(b, s->somente_leitura);
        json_builder_set_member_name(b, "de_replanejamento"); json_builder_add_boolean_value(b, s->de_replanejamento);
        if (com_estado) {
            json_builder_set_member_name(b, "estado"); json_builder_add_string_value(b, agente_passo_estado_texto(s->estado));
            json_builder_set_member_name(b, "tempo_execucao_ms"); json_builder_add_int_value(b, s->duracao_ms);
            b_strv(b, "evidencias", s->evidencias);
            b_strv(b, "limitacoes", s->limitacoes);
            json_builder_set_member_name(b, "erro");
            if (s->erro) json_builder_add_string_value(b, s->erro); else json_builder_add_null_value(b);
            if (s->dados_json) b_bruto(b, "dados", s->dados_json);
        }
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    b_strv(b, "criterios_sucesso", p->criterios_sucesso);
    b_strv(b, "capacidades", p->capacidades);
    json_builder_set_member_name(b, "risco"); json_builder_add_string_value(b, p->risco ? p->risco : "?");
    json_builder_set_member_name(b, "requer_aprovacao"); json_builder_add_boolean_value(b, p->requer_aprovacao);
    json_builder_set_member_name(b, "somente_leitura"); json_builder_add_boolean_value(b, p->somente_leitura);
    json_builder_set_member_name(b, "orcamento_passos"); json_builder_add_int_value(b, p->orcamento_passos);
    json_builder_set_member_name(b, "orcamento_tempo_ms"); json_builder_add_int_value(b, p->orcamento_tempo_ms);
    json_builder_set_member_name(b, "replanejamentos"); json_builder_add_int_value(b, p->replanejamentos);
    b_strv(b, "limitacoes", p->limitacoes);
    json_builder_end_object(b);
    JsonNode *raiz = json_builder_get_root(b);
    JsonGenerator *g = json_generator_new();
    json_generator_set_root(g, raiz);
    char *s = json_generator_to_data(g, NULL);
    g_object_unref(g);
    json_node_unref(raiz);
    g_object_unref(b);
    return s;
}

/* ------------------------------------------------- planejador de tarefas --- */

/* minúsculas, sem acento, só letras/dígitos separados por um espaço, com
 * espaço nas pontas (para casar " palavra" no começo também) */
static char *normalizar_pedido(const char *texto)
{
    char *nfd = g_utf8_normalize(texto ? texto : "", -1, G_NORMALIZE_NFD);
    GString *o = g_string_new(" ");
    for (const char *p = nfd ? nfd : ""; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_ismark(ch)) continue;
        if (ch < 128 && g_ascii_isalnum((char)ch)) g_string_append_c(o, g_ascii_tolower((char)ch));
        else if (o->str[o->len - 1] != ' ') g_string_append_c(o, ' ');
    }
    if (o->str[o->len - 1] != ' ') g_string_append_c(o, ' ');
    g_free(nfd);
    return g_string_free(o, FALSE);
}

static gboolean tem_algum(const char *t, const char *const *v)
{
    for (int i = 0; v[i]; i++)
        if (strstr(t, v[i])) return TRUE;
    return FALSE;
}

#define L(...) ((const char *const[]){ __VA_ARGS__, NULL })

/* Calculadora local limitada a expressões numéricas: sem eval, comandos ou
 * acesso a ferramentas. */
typedef struct { const char *p; gboolean valido; } Expressao;
static double expressao_soma(Expressao *e);

static void expressao_espacos(Expressao *e)
{
    while (g_ascii_isspace(*e->p)) e->p++;
}

static double expressao_primaria(Expressao *e)
{
    expressao_espacos(e);
    if (*e->p == '(') {
        e->p++;
        double v = expressao_soma(e);
        expressao_espacos(e);
        if (*e->p != ')') e->valido = FALSE;
        else e->p++;
        return v;
    }
    const char *ini = e->p;
    while (g_ascii_isdigit(*e->p)) e->p++;
    if (*e->p == '.' || *e->p == ',') {
        e->p++;
        while (g_ascii_isdigit(*e->p)) e->p++;
    }
    if (e->p == ini) { e->valido = FALSE; return 0; }
    char *numero = g_strndup(ini, (gsize)(e->p - ini));
    for (char *p = numero; *p; p++) if (*p == ',') *p = '.';
    double v = g_ascii_strtod(numero, NULL);
    g_free(numero);
    if (!isfinite(v)) e->valido = FALSE;
    return v;
}

static double expressao_unaria(Expressao *e)
{
    expressao_espacos(e);
    if (*e->p == '+') { e->p++; return expressao_unaria(e); }
    if (*e->p == '-') { e->p++; return -expressao_unaria(e); }
    return expressao_primaria(e);
}

static gboolean expressao_operador_texto(Expressao *e, const char *texto)
{
    expressao_espacos(e);
    gsize n = strlen(texto);
    if (!g_str_has_prefix(e->p, texto) ||
        (e->p[n] && !g_ascii_isspace(e->p[n]) && !strchr("?.,!", e->p[n])))
        return FALSE;
    e->p += n;
    return TRUE;
}

static double expressao_potencia(Expressao *e)
{
    double v = expressao_unaria(e);
    expressao_espacos(e);
    if (*e->p == '^') {
        e->p++;
        v = pow(v, expressao_potencia(e));
        if (!isfinite(v)) e->valido = FALSE;
    }
    return v;
}

static double expressao_produto(Expressao *e)
{
    double v = expressao_potencia(e);
    for (;;) {
        expressao_espacos(e);
        char op = *e->p;
        if (op == '*' || op == '/' || op == '%') e->p++;
        else if (expressao_operador_texto(e, "vezes") || expressao_operador_texto(e, "multiplicado por")) op = '*';
        else if (expressao_operador_texto(e, "dividido por")) op = '/';
        else return v;
        double b = expressao_potencia(e);
        if ((op == '/' || op == '%') && b == 0) { e->valido = FALSE; return 0; }
        v = op == '*' ? v * b : op == '/' ? v / b : fmod(v, b);
        if (!isfinite(v)) e->valido = FALSE;
    }
}

static double expressao_soma(Expressao *e)
{
    double v = expressao_produto(e);
    for (;;) {
        expressao_espacos(e);
        char op = *e->p;
        if (op == '+' || op == '-') e->p++;
        else if (expressao_operador_texto(e, "mais")) op = '+';
        else if (expressao_operador_texto(e, "menos")) op = '-';
        else return v;
        double b = expressao_produto(e);
        v = op == '+' ? v + b : v - b;
        if (!isfinite(v)) e->valido = FALSE;
    }
}

static gboolean resposta_matematica(const char *pedido, const char *normalizado, char **resposta)
{
    gboolean raiz = strstr(normalizado, " raiz quadrada") != NULL;
    gboolean pedido_calculo = raiz || tem_algum(normalizado,
        L(" quanto e ", " quanto eh ", " calcule ", " resolva ", " resultado de ", " soma de ", " multiplique "));
    if (!pedido_calculo) return FALSE;
    const char *inicio = pedido;
    while (*inicio && !g_ascii_isdigit(*inicio) && *inicio != '(' && *inicio != '+' && *inicio != '-') inicio++;
    if (!*inicio) return FALSE;
    Expressao e = { inicio, TRUE };
    double valor = expressao_soma(&e);
    expressao_espacos(&e);
    if (!e.valido || (*e.p && !strchr("?.,!", *e.p))) return FALSE;
    if (raiz) {
        if (valor < 0) {
            *resposta = g_strdup("A raiz quadrada de um número negativo não é um número real.");
            return TRUE;
        }
        valor = sqrt(valor);
    }
    char formatado[96];
    g_ascii_formatd(formatado, sizeof formatado, "%.12g", valor);
    for (char *p = formatado; *p; p++) if (*p == '.') *p = ',';
    *resposta = raiz ? g_strdup_printf("A raiz quadrada é %s.", formatado) :
                       g_strdup_printf("O resultado é %s.", formatado);
    return TRUE;
}

/* "os 10 processos", "top 5": só o número que a pessoa disse nessa posição */
static int limite_pedido(const char *t)
{
    for (const char *p = t; *p; p++) {
        if (!g_ascii_isdigit(*p) || (p > t && p[-1] != ' ')) continue;
        int v = 0, nd = 0;
        const char *q = p;
        while (g_ascii_isdigit(*q) && nd < 4) { v = v * 10 + (*q - '0'); q++; nd++; }
        if (*q == ' ' && (g_str_has_prefix(q, " processo") || g_str_has_prefix(q, " programa") ||
                          g_str_has_prefix(q, " maiores") || g_str_has_prefix(q, " principais")) && v >= 1 && v <= 100)
            return v;
        p = q;
    }
    return 0;
}

static const char *const LIMITACOES_MALWARE[] = {
    "não existe ferramenta de detecção de malware nesta versão: nenhuma assinatura, checksum ou indicador conhecido foi consultado",
    "não foram verificados: pacotes instalados e sua integridade, arquivos alterados recentemente, serviços ativos, mecanismos de persistência (autostart, timers, cron), módulos de kernel e conexões de rede",
    "consumo de CPU ou memória não é indício de malware por si só, e um processo malicioso pode consumir muito pouco",
    "só o nome curto de cada processo é lido (nunca a linha de comando), então um processo pode se apresentar com um nome comum",
    NULL,
};

/* palavra sem acento, minúscula, sem pontuação nas pontas */
static char *palavra_normalizada(const char *w)
{
    char *n = normalizar_pedido(w);
    g_strstrip(n);
    return n;
}

/* Tira do começo do pedido o que é conversa, não assunto ("me conte com
 * detalhes a", "pesquise na internet sobre o", "você pode me dizer") e a
 * pontuação final — o buscador recebe só o assunto: "evolução da linguagem
 * Haskell". Se não sobrar nada, devolve o pedido como veio. */
static char *consulta_limpa(const char *pedido)
{
    static const char *const ENCHIMENTO[] = {
        "me", "nos", "conte", "conta", "contar", "fale", "fala", "falar", "diga", "diz", "dizer", "explique", "explica",
        "explicar", "descreva", "descreve", "mostre", "mostra", "resuma", "resume", "pesquise", "pesquisa", "pesquisar",
        "busque", "busca", "buscar", "procure", "procura", "procurar", "encontre", "informe", "liste", "lista", "na", "no",
        "em", "pela", "pelo", "internet", "web", "google", "sobre", "a", "o", "os", "as", "um", "uma", "com", "mais",
        "detalhes", "detalhadamente", "detalhe", "detalhada", "detalhado", "brevemente", "rapidamente", "resumidamente",
        "por", "favor", "lia", "voce", "vc", "pode", "poderia", "consegue", "conseguiria", "quero", "queria", "gostaria",
        "de", "saber", "entao", "ok", "oi", "ola", "ei", "ai", "agora", "tudo", "que", "sabe", "sei", "algo", "coisa", NULL };
    char **palavras = g_strsplit_set(pedido ? pedido : "", " \t\n", -1);
    guint i = 0;
    for (; palavras[i]; i++) {
        if (!*palavras[i]) continue;
        char *n = palavra_normalizada(palavras[i]);
        gboolean enchimento = !*n || g_strv_contains(ENCHIMENTO, n);
        g_free(n);
        if (!enchimento) break;
    }
    GString *o = g_string_new(NULL);
    for (guint k = i; palavras[k]; k++) {
        if (!*palavras[k]) continue;
        if (o->len) g_string_append_c(o, ' ');
        g_string_append(o, palavras[k]);
    }
    g_strfreev(palavras);
    /* "… por favor", pontuação final */
    char *t = g_string_free(o, FALSE);
    char *min = g_utf8_strdown(t, -1);
    if (g_str_has_suffix(min, " por favor")) t[strlen(t) - strlen(" por favor")] = '\0';
    g_free(min);
    gsize n = strlen(t);
    while (n && strchr("?.!,;: ", t[n - 1])) t[--n] = '\0';
    if (n < 2) { g_free(t); t = g_strdup(pedido ? pedido : ""); g_strstrip(t); }
    return t;
}

/* pergunta de continuação ("e a versão atual dela?", "quando isso
 * aconteceu?"): curta e apoiada no assunto anterior */
static gboolean pedido_de_continuacao(const char *limpa)
{
    char *t = normalizar_pedido(limpa);
    gboolean anafora = g_str_has_prefix(t, " e ") ||
        tem_algum(t, L(" ele ", " ela ", " eles ", " elas ", " isso ", " disso ", " nisso ", " isto ", " dele ", " dela ",
                       " deles ", " delas ", " esse ", " essa ", " desse ", " dessa ", " este ", " deste ", " desta ",
                       " mesmo ", " mesma ", " anterior ", " seu ", " sua ", " seus ", " suas "));
    int palavras = 0;
    for (const char *p = t; *p; p++) if (*p == ' ' && p[1]) palavras++;
    g_free(t);
    return anafora && palavras <= 7;
}

/* última pergunta da pessoa no contexto (linhas "Usuário: …" que o
 * lia-chat envia), diferente do pedido atual; nunca texto da Lia */
static char *pergunta_anterior(const char *contexto, const char *pedido)
{
    char *ultima = NULL;
    char **linhas = g_strsplit(contexto ? contexto : "", "\n", -1);
    for (guint i = 0; linhas[i]; i++) {
        if (!g_str_has_prefix(linhas[i], "Usuário: ")) continue;
        const char *q = linhas[i] + strlen("Usuário: ");
        if (!*q || (pedido && !strcmp(q, pedido))) continue;
        g_free(ultima);
        ultima = g_strdup(q);
    }
    g_strfreev(linhas);
    return ultima;
}

static char *web_parametros(const char *pedido, const char *contexto)
{
    const gsize limite = 500;
    char *consulta = consulta_limpa(pedido);
    if (pedido_de_continuacao(consulta)) {
        char *anterior = pergunta_anterior(contexto, pedido);
        if (anterior) {
            char *assunto = consulta_limpa(anterior);
            if (strlen(assunto) + strlen(consulta) + 1 < limite) {
                char *junto = g_strdup_printf("%s %s", assunto, consulta);
                g_free(consulta);
                consulta = junto;
            }
            g_free(assunto);
            g_free(anterior);
        }
    }
    char *normalizado = normalizar_pedido(consulta);
    gboolean pergunta_proxima = strstr(normalizado, " proxima ") || strstr(normalizado, " proximo ");
    g_free(normalizado);
    if (pergunta_proxima && strlen(consulta) + strlen(" próxima edição datas calendário") < limite) {
        char *c2 = g_strconcat(consulta, " próxima edição datas calendário", NULL);
        g_free(consulta);
        consulta = c2;
    }
    if (strlen(consulta) > limite) {
        const char *corte = g_utf8_find_prev_char(consulta, consulta + limite);
        if (corte) consulta[corte - consulta] = '\0';
    }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "consulta");
    json_builder_add_string_value(b, consulta);
    json_builder_set_member_name(b, "limite");
    json_builder_add_int_value(b, 6);
    json_builder_set_member_name(b, "abrir_fontes");
    json_builder_add_int_value(b, 3);
    json_builder_end_object(b);
    JsonNode *n = json_builder_get_root(b);
    JsonGenerator *g = json_generator_new();
    json_generator_set_root(g, n);
    char *s = json_generator_to_data(g, NULL);
    json_node_unref(n); g_object_unref(g); g_object_unref(b); g_free(consulta);
    return s;
}

static char *web_url_parametros(const char *pedido)
{
    const char *ini = strstr(pedido ? pedido : "", "https://");
    if (!ini) return NULL;
    const char *fim = ini;
    while (*fim && !g_ascii_isspace(*fim) && *fim != ')' && *fim != ']' && *fim != '"') fim++;
    char *url = g_strndup(ini, (gsize)(fim - ini));
    JsonBuilder *b = json_builder_new(); json_builder_begin_object(b);
    json_builder_set_member_name(b, "url"); json_builder_add_string_value(b, url);
    json_builder_end_object(b); g_free(url);
    JsonNode *n = json_builder_get_root(b); JsonGenerator *g = json_generator_new(); json_generator_set_root(g, n);
    char *s = json_generator_to_data(g, NULL); json_node_unref(n); g_object_unref(g); g_object_unref(b); return s;
}

AgentePlano *agente_planejar_com_contexto(const char *pedido, const char *contexto,
                                          char **texto_direto, AgenteCompat *compat)
{
    if (texto_direto) *texto_direto = NULL;
    if (compat) *compat = COMPAT_COMPATIVEL;
    char *t = normalizar_pedido(pedido);
    AgentePlano *p = NULL;
    char *resposta_calculo = NULL;
    if (resposta_matematica(pedido ? pedido : "", t, &resposta_calculo)) {
        if (compat) *compat = COMPAT_COMPATIVEL;
        if (texto_direto) *texto_direto = resposta_calculo;
        else g_free(resposta_calculo);
        g_free(t);
        return NULL;
    }
    /* boas práticas de programação: catálogo local (projeto/praticas.h),
     * antes da web — "boas práticas de C" não é pergunta para a internet */
    if (tem_algum(t, L(" boas praticas", " boa pratica", " melhores praticas", " praticas de ", " praticas para ", " checklist de",
                      " checklist para", " qual a forma correta", " como fazer direito", " do jeito certo", " padroes de codigo",
                      " principios de"))) {
        JsonBuilder *jb = json_builder_new();
        json_builder_begin_object(jb);
        json_builder_set_member_name(jb, "tarefa");
        json_builder_add_string_value(jb, pedido);
        json_builder_end_object(jb);
        JsonGenerator *jg = json_generator_new();
        JsonNode *raiz = json_builder_get_root(jb);
        json_generator_set_root(jg, raiz);
        char *params = json_generator_to_data(jg, NULL);
        json_node_unref(raiz);
        g_object_unref(jg);
        g_object_unref(jb);
        AgentePlano *pp = agente_plano_novo("consultar o catálogo local de boas práticas para esta tarefa", "boas_praticas");
        agente_plano_acrescentar(pp, NULL, "praticas.consultar", params, NULL, "boas práticas relevantes", L("praticas_selecionadas"), 0, FALSE);
        g_free(params);
        pp->criterios_sucesso = strv_de(L("praticas_selecionadas"));
        if (compat) *compat = COMPAT_COMPATIVEL;
        g_free(t);
        return pp;
    }
    /* arquivos do projeto: pedidos explícitos (ler, listar, procurar, criar,
     * substituir, apagar, mover...) viram um plano de um passo; a escrita
     * continua exigindo aprovação com prévia no daemon */
    {
        char *pergunta_arq = NULL;
        AgentePlano *pa = agente_planejar_arquivos(pedido, &pergunta_arq);
        if (pa || pergunta_arq) {
            if (compat) *compat = pa ? COMPAT_COMPATIVEL : COMPAT_ESCLARECER;
            if (texto_direto) *texto_direto = pergunta_arq; else g_free(pergunta_arq);
            g_free(t);
            return pa;
        }
    }
    int limite = limite_pedido(t);
    char lim[32];
    g_snprintf(lim, sizeof lim, "{\"limite\":%d}", limite ? limite : 10);

    gboolean cpu = tem_algum(t, L(" cpu", " processador", " processamento"));
    gboolean memoria = tem_algum(t, L(" memoria", " ram "));
    gboolean malware = tem_algum(t, L(" virus", " malware", " infectad", " trojan", " ransomware", " rootkit", " spyware", " suspeit", " invadid", " hackead"));
    /* local: palavras de sistema, ou uma intenção que o roteador de
     * consultas simples reconhece (rede/discos/PCI, kernel, recursos,
     * programas instalados, espaço em disco...) — essas nunca vão para a web */
    Rota rota;
    roteador_classificar(pedido ? pedido : "", &rota);
    gboolean local_roteador = rota.intencao && (rota.sem_ferramenta || rota.esclarecer ||
                              (rota.ferramenta && (g_str_has_prefix(rota.ferramenta, "sistema.") || g_str_has_prefix(rota.ferramenta, "tools."))));
    /* tarefas sobre ESTA máquina (lentidão, consumo anormal, relatório de
     * recursos, estado do sistema): "por que o Python é lento?" é pergunta
     * de conhecimento; "por que o sistema está lento?" é diagnóstico local */
    gboolean sobre_a_maquina = tem_algum(t, L(" sistema", " computador", " maquina", " pc ", " notebook", " linux", " arch",
                                              " recursos atuais", " meus recursos"));
    /* "por que está lento?", sem assunto nenhum, também é sobre esta máquina */
    gboolean sem_assunto = TRUE;
    {
        static const char *const VAZIAS[] = { "por", "que", "porque", "o", "a", "os", "as", "e", "esta", "estao", "ta", "tao",
            "muito", "meu", "minha", "tudo", "ficou", "anda", "andando", "sendo", "lento", "lenta", "lentos", "travando",
            "devagar", "agora", "hoje", "isso", "isto", "assim", "tanto", "demais", "ultimamente", "de", "repente", NULL };
        char **ps = g_strsplit(t, " ", -1);
        for (guint i = 0; ps[i] && sem_assunto; i++) if (*ps[i] && !g_strv_contains(VAZIAS, ps[i])) sem_assunto = FALSE;
        g_strfreev(ps);
    }
    gboolean tarefa_local = (sobre_a_maquina || sem_assunto) &&
                            tem_algum(t, L(" lent", " travando", " devagar", " desempenho", " engasg", " anormal", " anomal",
                                           " incomum", " excessiv", " fora do normal", " relatorio", " estado atual",
                                           " estado geral", " estado do ", " situacao atual", " resum", " evidenc"));
    gboolean local = cpu || memoria || malware || local_roteador || tarefa_local ||
                     tem_algum(t, L(" processo", " inventario", " dispositivo", " hardware", " computador", " pc ",
                                    " maquina", " sistema operacional", " kernel", " uptime", " swap", " carga media",
                                    " este sistema", " meu sistema", " minha maquina"));
    gboolean pergunta = (pedido && strchr(pedido, '?')) ||
                        tem_algum(t, L(" qual ", " quais ", " quem ", " quando ", " onde ", " quanto ", " quanta ", " quantos ",
                                       " como ", " por que ", " porque ", " o que ", " existe ", " existem "));
    /* pedido de conhecimento: verbo de pedido de informação ou assunto
     * claramente factual — "me conte com detalhes a evolução de X" não tem
     * interrogação, mas pede conhecimento que só a web confirma */
    gboolean pedido_informativo = tem_algum(t, L(" pesquis", " internet", " noticia", " cotacao", " dolar", " euro", " libra",
                                                 " preco", " fonte", " site", " online", " mercado", " conte", " conta ", " fale",
                                                 " fala ", " explique", " explica", " descreva", " descreve", " resuma", " resume ",
                                                 " defina", " diga ", " me diz", " informe", " busque", " procure", " encontre",
                                                 " historia", " evolucao", " detalh", " significado", " diferenca", " vantage",
                                                 " desvantage", " ultimas ", " lancament", " novidade", " estado atual"));
    /* conversa social curta ("oi", "obrigado", "quem é você?") não é pesquisa */
    int n_palavras = 0;
    for (const char *q = t; *q; q++) if (*q == ' ' && q[1]) n_palavras++;
    gboolean social = n_palavras <= 6 &&
                      tem_algum(t, L(" oi ", " ola ", " bom dia", " boa tarde", " boa noite", " obrigad", " valeu ", " tudo bem",
                                     " quem e voce", " quem voce e", " o que voce faz", " o que voce pode", " seu nome", " teste ",
                                     /* pedido de ajuda sem assunto: conversa, não pesquisa */
                                     " pode me ajudar", " poderia me ajudar", " me ajuda ", " me ajude ", " preciso de ajuda",
                                     " voce pode ajudar", " consegue me ajudar"));
    gboolean web = !social && !local &&
                   (pedido_informativo || pergunta || (pedido && (strstr(pedido, "https://") || strstr(pedido, "http://"))));

    if (malware) {
        if (tem_algum(t, L(" expli", " insuficient", " quais dados", " por que nao", " o que falta"))) {
            if (compat) *compat = COMPAT_INEXISTENTE;
            if (texto_direto) {
                GString *s = g_string_new("Conclusão: com as ferramentas desta versão não é possível afirmar que há malware, nem que não há. "
                                          "Nada foi executado.\n\nPor que os dados disponíveis são insuficientes:\n");
                for (int i = 0; LIMITACOES_MALWARE[i]; i++)
                    g_string_append_printf(s, "  - %s;\n", LIMITACOES_MALWARE[i]);
                g_string_append(s, "\nO que seria necessário (ainda não existe aqui): inventário de pacotes com verificação de integridade, "
                                   "lista de arquivos alterados recentemente, serviços e mecanismos de persistência, e comparação com assinaturas "
                                   "ou indicadores conhecidos. Mesmo com isso, o resultado seria \"indícios encontrados\" ou \"não foi possível "
                                   "concluir\" — nunca uma garantia.\n\nPróximo passo: posso coletar, só com leitura, os processos que mais usam CPU "
                                   "e memória, a carga e a memória — peça \"colete evidências de processos suspeitos\".");
                *texto_direto = g_string_free(s, FALSE);
            }
        } else if (tem_algum(t, L(" process", " colet", " evidenc", " list", " investig", " indicio", " mostr"))) {
            if (compat) *compat = COMPAT_PARCIAL;
            p = agente_plano_novo("coletar evidências de leitura sobre processos e recursos (não detecta malware)", "evidencias_sobre_malware");
            p->compatibilidade = COMPAT_PARCIAL;
            p->limitacoes = strv_de(LIMITACOES_MALWARE);
            agente_plano_acrescentar(p, NULL, "sistema.recursos", "{}", NULL, "carga, memória e swap atuais",
                                     L("carga_media", "memoria_disponivel", "cpu_nucleos"), 0, FALSE);
            agente_plano_acrescentar(p, NULL, "sistema.processos_top_cpu", lim, NULL, "processos que mais usam CPU", L("lista_processos_cpu"), 0, FALSE);
            agente_plano_acrescentar(p, NULL, "sistema.processos_top_memoria", lim, NULL, "processos que mais usam memória", L("lista_processos_memoria"), 0, FALSE);
            p->criterios_sucesso = strv_de(L("lista_processos_cpu", "lista_processos_memoria", "carga_media"));
        } else {
            if (compat) *compat = COMPAT_INEXISTENTE;
            if (texto_direto)
                *texto_direto = g_strdup(
                    "Não há ferramenta de detecção de malware disponível nesta versão, então não é possível afirmar que esta máquina "
                    "está infectada, nem que está livre de malware. Posso coletar evidências somente de leitura, como os processos que "
                    "mais usam CPU e memória, a carga e a memória, mas isso não prova que o sistema está livre de malware. "
                    "Nada foi executado. Se quiser essa coleta limitada, peça: \"colete evidências de processos suspeitos\".");
        }
    } else if (web) {
        if (compat) *compat = COMPAT_COMPATIVEL;
        char *url_params = web_url_parametros(pedido);
        gboolean abrir = url_params != NULL;
        p = agente_plano_novo(abrir ? "abrir a fonte HTTPS indicada e responder com base nela"
                                    : "pesquisar na web, ler as melhores fontes e responder com base nelas",
                              abrir ? "abrir_web" : "pesquisa_web");
        char *params = abrir ? url_params : web_parametros(pedido, contexto);
        agente_plano_acrescentar(p, NULL, abrir ? "web.abrir" : "web.pesquisar", params, NULL,
                                 abrir ? "ler a página pública indicada" : "buscar e ler fontes atuais na internet",
                                 abrir ? L("conteudo_web") : L("resultados_web"), 35000, FALSE);
        g_free(params);
        p->criterios_sucesso = strv_de((const char *[]) { abrir ? "conteudo_web" : "resultados_web", NULL });
        p->limitacoes = strv_de((const char *[]) {
            abrir ? "a URL indicada é aberta na internet pública" :
                    "a consulta é enviada a um mecanismo de busca externo e até 3 das páginas encontradas são abertas",
            "a resposta é tão confiável quanto as fontes lidas: confira as datas e a credibilidade de cada uma",
            NULL });
    } else if (tem_algum(t, L(" compar")) && cpu && memoria) {
        p = agente_plano_novo("comparar os processos que mais usam CPU com os que mais usam memória", "comparar_cpu_memoria");
        agente_plano_acrescentar(p, NULL, "sistema.processos_top_cpu", lim, NULL, "ranking por CPU", L("lista_processos_cpu"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.processos_top_memoria", lim, NULL, "ranking por memória", L("lista_processos_memoria"), 0, FALSE);
    } else if (tem_algum(t, L(" lent", " travando", " devagar", " desempenho", " engasg"))) {
        p = agente_plano_novo("investigar por que o sistema pode estar lento agora", "investigar_lentidao");
        agente_plano_acrescentar(p, NULL, "sistema.recursos", "{}", NULL, "carga, memória e swap atuais",
                                 L("carga_media", "memoria_disponivel", "swap", "cpu_nucleos"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.processos_top_cpu", lim, NULL, "quem está usando CPU", L("lista_processos_cpu"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.processos_top_memoria", lim, NULL, "quem está usando memória", L("lista_processos_memoria"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.dispositivos", "{\"classe\":\"bloco\"}", NULL, "quais discos existem", L("dispositivos_bloco"), 0, TRUE);
        p->criterios_sucesso = strv_de(L("carga_media", "cpu_nucleos", "memoria_disponivel", "swap", "lista_processos_cpu", "lista_processos_memoria"));
        p->limitacoes = strv_de(L("não há medição de E/S de disco, de rede nem de temperatura nesta versão: lentidão causada por esses fatores não aparece aqui",
                                   "as medições são um retrato de agora (CPU numa janela de 200 ms): um pico anterior não é visto"));
    } else if (tem_algum(t, L(" anormal", " anomal", " incomum", " excessiv", " fora do normal"))) {
        p = agente_plano_novo("verificar se algum processo consome recursos de forma anormal", "consumo_anormal");
        agente_plano_acrescentar(p, NULL, "sistema.recursos", "{}", NULL, "capacidade e uso atuais", L("carga_media", "memoria_disponivel", "cpu_nucleos", "memoria_total"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.processos_top_cpu", lim, NULL, "ranking por CPU", L("lista_processos_cpu"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.processos_top_memoria", lim, NULL, "ranking por memória", L("lista_processos_memoria"), 0, FALSE);
        p->criterios_sucesso = strv_de(L("carga_media", "cpu_nucleos", "memoria_disponivel", "lista_processos_cpu", "lista_processos_memoria"));
        p->limitacoes = strv_de(L("\"anormal\" aqui é só um critério numérico fixo (um processo ocupando cerca de um núcleo inteiro, ou mais de 25 % da RAM): não há histórico para comparar com o comportamento habitual desta máquina"));
    } else if (tem_algum(t, L(" inventario")) && tem_algum(t, L(" rede", " pci", " bloco", " hardware", " dispositiv", " disco"))) {
        p = agente_plano_novo("inventário somente leitura de rede, PCI e dispositivos de bloco", "inventario_de_hardware");
        agente_plano_acrescentar(p, NULL, "sistema.dispositivos", "{\"classe\":\"rede\"}", NULL, "interfaces de rede", L("dispositivos_rede"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.dispositivos", "{\"classe\":\"pci\"}", NULL, "dispositivos PCI", L("dispositivos_pci"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.dispositivos", "{\"classe\":\"bloco\"}", NULL, "dispositivos de bloco", L("dispositivos_bloco"), 0, FALSE);
        p->limitacoes = strv_de(L("os dispositivos PCI são identificados só pelos códigos numéricos de fabricante e dispositivo (não há base de nomes nesta versão)",
                                   "dispositivos USB e tamanhos/uso dos discos não são lidos nesta versão"));
    } else if (tem_algum(t, L(" relatorio"))) {
        p = agente_plano_novo("relatório dos recursos atuais da máquina", "relatorio_de_recursos");
        agente_plano_acrescentar(p, NULL, "sistema.diagnosticar", "{}", NULL, "identificação: distribuição, kernel, CPU",
                                 L("distribuicao", "kernel", "cpu_modelo", "cpu_nucleos"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.recursos", "{}", NULL, "RAM, swap, uptime e carga média",
                                 L("memoria_total", "memoria_disponivel", "swap", "uptime", "carga_media"), 0, FALSE);
        p->criterios_sucesso = strv_de(L("cpu_modelo", "cpu_nucleos", "memoria_total", "memoria_disponivel", "swap", "uptime", "carga_media"));
    } else if (tem_algum(t, L(" estado atual", " estado geral", " estado do ", " situacao atual")) ||
               (tem_algum(t, L(" resum")) && tem_algum(t, L(" arch", " sistema ", " linux")))) {
        p = agente_plano_novo("resumir o estado atual do sistema com dados coletados agora", "estado_do_sistema");
        agente_plano_acrescentar(p, NULL, "sistema.diagnosticar", "{}", NULL, "identificação: distribuição, kernel, CPU", L("distribuicao", "kernel", "cpu_modelo"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.recursos", "{}", NULL, "RAM, swap, uptime e carga média",
                                 L("memoria_disponivel", "swap", "uptime", "carga_media", "cpu_nucleos"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.processos_top_cpu", "{\"limite\":5}", NULL, "processos que mais usam CPU", L("lista_processos_cpu"), 0, FALSE);
        agente_plano_acrescentar(p, NULL, "sistema.processos_top_memoria", "{\"limite\":5}", NULL, "processos que mais usam memória", L("lista_processos_memoria"), 0, FALSE);
        p->criterios_sucesso = strv_de(L("distribuicao", "kernel", "memoria_disponivel", "carga_media", "uptime", "lista_processos_cpu", "lista_processos_memoria"));
    }
    g_free(t);
    return p;
}

AgentePlano *agente_planejar(const char *pedido, char **texto_direto, AgenteCompat *compat)
{
    return agente_planejar_com_contexto(pedido, NULL, texto_direto, compat);
}

/* -------------------------------------------------------- execução/ordem --- */

int agente_passos_prontos(AgentePlano *p, int *indices, int max, int *pulados)
{
    int n = 0, executando = 0, n_pulados = 0;
    gboolean nao_leitura_executando = FALSE;
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        if (s->estado == PASSO_EXECUTANDO) {
            executando++;
            if (!s->somente_leitura) nao_leitura_executando = TRUE;
        }
    }
    /* propaga "pulado" até estabilizar (cadeias de dependência) */
    for (gboolean mudou = TRUE; mudou;) {
        mudou = FALSE;
        for (guint i = 0; i < p->passos->len; i++) {
            AgentePasso *s = g_ptr_array_index(p->passos, i);
            if (s->estado != PASSO_AGUARDANDO) continue;
            for (int k = 0; s->depende_de[k]; k++) {
                AgentePasso *d = passo_por_id(p, s->depende_de[k]);
                if (d && (d->estado == PASSO_FALHA || d->estado == PASSO_CANCELADO || d->estado == PASSO_PULADO)) {
                    s->estado = PASSO_PULADO;
                    g_free(s->erro);
                    s->erro = g_strdup_printf("não executado: depende de '%s', que terminou em %s", d->id, agente_passo_estado_texto(d->estado));
                    n_pulados++;
                    mudou = TRUE;
                    break;
                }
            }
        }
    }
    if (pulados) *pulados = n_pulados;
    if (nao_leitura_executando) return 0; /* passo que não é leitura pura roda sozinho */
    for (guint i = 0; i < p->passos->len && n < max; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        if (s->estado != PASSO_AGUARDANDO) continue;
        gboolean pronto = TRUE;
        for (int k = 0; s->depende_de[k] && pronto; k++) {
            AgentePasso *d = passo_por_id(p, s->depende_de[k]);
            pronto = d && d->estado == PASSO_SUCESSO;
        }
        if (!pronto) continue;
        if (!s->somente_leitura) {
            /* escrita/admin/hardware: nunca em paralelo com nada */
            if (executando == 0 && n == 0) indices[n++] = (int)i;
            break;
        }
        if (executando + n >= AGENTE_MAX_PARALELO) break;
        indices[n++] = (int)i;
    }
    return n;
}

/* ----------------------------------------------------------- observação --- */

/* carga útil: {"saida": ...} das ferramentas do executor real, senão a raiz */
static JsonNode *carga_util(JsonParser *jp, const char *dados_json)
{
    if (!dados_json || !json_parser_load_from_data(jp, dados_json, -1, NULL) || !json_parser_get_root(jp))
        return NULL;
    JsonNode *raiz = json_parser_get_root(jp);
    if (JSON_NODE_TYPE(raiz) == JSON_NODE_OBJECT && json_object_has_member(json_node_get_object(raiz), "saida"))
        return json_object_get_member(json_node_get_object(raiz), "saida");
    return raiz;
}

/* lista de processos válida: array não vazio de objetos com pid inteiro,
 * nome texto e o campo de métrica inteiro. *n recebe o tamanho. */
static gboolean lista_processos_valida(JsonObject *d, const char *metrica, guint *n)
{
    JsonArray *a = jarr(d, "processos");
    *n = a ? json_array_get_length(a) : 0;
    if (!a || *n == 0) return FALSE;
    for (guint i = 0; i < *n; i++) {
        JsonNode *el = json_array_get_element(a, i);
        if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) return FALSE;
        JsonObject *o = json_node_get_object(el);
        gint64 pid, m;
        if (!jint(o, "pid", &pid) || pid <= 0 || !jstr(o, "nome") || !jint(o, metrica, &m) || m < 0) return FALSE;
    }
    return TRUE;
}

char **agente_extrair_evidencias(const char *ferramenta, const char *parametros_json,
                                  const char *dados_json, char ***limitacoes)
{
    char **ev = NULL, **lim = NULL;
    const char *f = ferramenta ? ferramenta : "";
    JsonParser *jp = json_parser_new();
    JsonNode *carga = carga_util(jp, dados_json);
    JsonObject *d = carga && JSON_NODE_TYPE(carga) == JSON_NODE_OBJECT ? json_node_get_object(carga) : NULL;
    gint64 v;

    if (!carga) {
        lim = strv_acrescentar(lim, "a ferramenta não devolveu JSON legível");
    } else if (strcmp(f, "sistema.recursos") == 0 || strcmp(f, "sistema.diagnosticar") == 0) {
        JsonObject *mem = jobj(d, "memoria_kb"), *swap = jobj(d, "swap_kb"), *cpu = jobj(d, "cpu"), *carga_m = jobj(d, "carga_media_centesimos");
        if (jint(mem, "total", &v) && v > 0) ev = strv_acrescentar(ev, "memoria_total");
        else lim = strv_acrescentar(lim, "memória total ausente");
        gint64 total = v;
        if (jint(mem, "disponivel", &v) && v >= 0 && v <= total) ev = strv_acrescentar(ev, "memoria_disponivel");
        else lim = strv_acrescentar(lim, "memória disponível ausente ou incoerente");
        if (jint(cpu, "nucleos", &v) && v > 0) ev = strv_acrescentar(ev, "cpu_nucleos");
        if (jstr(cpu, "modelo") && *jstr(cpu, "modelo")) ev = strv_acrescentar(ev, "cpu_modelo");
        if (strcmp(f, "sistema.recursos") == 0) {
            gint64 st, sl;
            if (jint(swap, "total", &st) && jint(swap, "livre", &sl) && sl <= st) ev = strv_acrescentar(ev, "swap");
            else lim = strv_acrescentar(lim, "dados de swap ausentes ou incoerentes");
            if (jint(d, "uptime_segundos", &v) && v >= 0) ev = strv_acrescentar(ev, "uptime");
            else lim = strv_acrescentar(lim, "uptime ausente");
            gint64 c1, c5, c15;
            if (jint(carga_m, "1min", &c1) && jint(carga_m, "5min", &c5) && jint(carga_m, "15min", &c15))
                ev = strv_acrescentar(ev, "carga_media");
            else lim = strv_acrescentar(lim, "carga média ausente nesta coleta");
        } else {
            if (jstr(d, "distro") && *jstr(d, "distro")) ev = strv_acrescentar(ev, "distribuicao");
            if (jstr(jobj(d, "kernel"), "release")) ev = strv_acrescentar(ev, "kernel");
        }
    } else if (strcmp(f, "sistema.processos_top_cpu") == 0 || strcmp(f, "sistema.processos_top_memoria") == 0) {
        gboolean mem = strcmp(f, "sistema.processos_top_memoria") == 0;
        guint n = 0;
        if (d && lista_processos_valida(d, mem ? "rss_kb" : "cpu_pct_centesimos", &n)) {
            ev = strv_acrescentar(ev, mem ? "lista_processos_memoria" : "lista_processos_cpu");
            gint64 limite = 0, universo = 0;
            if (jint(d, "limite", &limite) && jint(d, mem ? "considerados" : "amostrados", &universo) &&
                (gint64)n < MIN(limite, universo))
                lim = strv_acrescentar(lim, "a lista de processos veio com menos itens do que o pedido (resultado parcial)");
            if (mem && jint(d, "memoria_total_kb", &v) && v > 0) ev = strv_acrescentar(ev, "memoria_total");
        } else {
            lim = strv_acrescentar(lim, "a ferramenta não devolveu uma lista de processos válida");
        }
    } else if (strcmp(f, "web.pesquisar") == 0) {
        JsonArray *resultados = d ? jarr(d, "resultados") : NULL;
        JsonArray *cotacoes = d ? jarr(d, "cotacoes") : NULL;
        if (cotacoes && json_array_get_length(cotacoes) > 0)
            ev = strv_acrescentar(ev, "cotacao_web");
        if (resultados && json_array_get_length(resultados) > 0)
            ev = strv_acrescentar(ev, "resultados_web");
        else
            lim = strv_acrescentar(lim, "a busca não devolveu resultados estruturados");
    } else if (strcmp(f, "praticas.consultar") == 0) {
        JsonArray *pa = d && json_object_has_member(d, "praticas") ? json_object_get_array_member(d, "praticas") : NULL;
        if (pa && json_array_get_length(pa) > 0) ev = strv_acrescentar(ev, "praticas_selecionadas");
        else lim = strv_acrescentar(lim, "nenhuma prática do catálogo se aplicou claramente ao pedido");
    } else if (g_str_has_prefix(f, "arquivo.")) {
        /* arquivos do projeto: a evidência é o próprio resultado da operação */
        if (!strcmp(f, "arquivo.ler")) ev = strv_acrescentar(ev, "conteudo_arquivo");
        else if (!strcmp(f, "arquivo.listar")) ev = strv_acrescentar(ev, "lista_arquivos");
        else if (!strcmp(f, "arquivo.buscar")) ev = strv_acrescentar(ev, "busca_arquivos");
        else if (!strcmp(f, "arquivo.info")) ev = strv_acrescentar(ev, "info_arquivo");
        else if (d && json_object_has_member(d, "operacao_id")) {
            JsonNode *cn = json_object_has_member(d, "completa") ? json_object_get_member(d, "completa") : NULL;
            if (!cn || json_node_get_boolean(cn)) ev = strv_acrescentar(ev, "arquivo_alterado");
            else lim = strv_acrescentar(lim, "a alteração parou no meio; veja o que foi feito no resultado");
        }
    } else if (strcmp(f, "web.abrir") == 0) {
        const char *conteudo = d ? jstr(d, "conteudo") : NULL;
        if (conteudo && *conteudo) ev = strv_acrescentar(ev, "conteudo_web");
        else lim = strv_acrescentar(lim, "a página não devolveu texto");
    } else if (strcmp(f, "sistema.dispositivos") == 0) {
        if (JSON_NODE_TYPE(carga) == JSON_NODE_ARRAY) {
            char *classe = NULL;
            JsonParser *pp = json_parser_new();
            if (parametros_json && json_parser_load_from_data(pp, parametros_json, -1, NULL) && json_parser_get_root(pp) &&
                JSON_NODE_TYPE(json_parser_get_root(pp)) == JSON_NODE_OBJECT)
                classe = g_strdup(jstr(json_node_get_object(json_parser_get_root(pp)), "classe"));
            g_object_unref(pp);
            static const char *const CL[] = { "rede", "bloco", "pci", NULL };
            for (int i = 0; CL[i]; i++) {
                if (classe && strcmp(classe, CL[i]) != 0) continue;
                char *e = g_strdup_printf("dispositivos_%s", CL[i]);
                ev = strv_acrescentar(ev, e);
                g_free(e);
            }
            if (json_array_get_length(json_node_get_array(carga)) == 0)
                lim = strv_acrescentar(lim, "a listagem de dispositivos veio vazia");
            g_free(classe);
        } else {
            lim = strv_acrescentar(lim, "a ferramenta não devolveu uma lista de dispositivos");
        }
    } else {
        /* ferramenta sem extrator próprio: o resultado em si é a evidência */
        char *e = g_strdup_printf("resultado:%s", f);
        ev = strv_acrescentar(ev, e);
        g_free(e);
    }
    g_object_unref(jp);
    if (limitacoes) *limitacoes = lim ? lim : g_new0(char *, 1);
    else g_strfreev(lim);
    return ev ? ev : g_new0(char *, 1);
}

/* ---------------------------------------------- verificação/replanejar --- */

static gboolean evidencia_presente(const AgentePlano *p, const char *e)
{
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        if (s->estado == PASSO_SUCESSO && strv_tem(s->evidencias, e)) return TRUE;
    }
    return FALSE;
}

AgenteVeredito agente_verificar(const AgentePlano *p, gboolean pode_replanejar, char ***ausentes)
{
    char **faltam = NULL;
    for (int i = 0; p->criterios_sucesso && p->criterios_sucesso[i]; i++)
        if (!evidencia_presente(p, p->criterios_sucesso[i]))
            faltam = strv_acrescentar(faltam, p->criterios_sucesso[i]);
    gboolean algum_dado = FALSE;
    for (guint i = 0; i < p->passos->len; i++)
        if (((AgentePasso *)g_ptr_array_index(p->passos, i))->estado == PASSO_SUCESSO) algum_dado = TRUE;
    AgenteVeredito v;
    if (!faltam) v = VEREDITO_SUCESSO;
    else if (pode_replanejar) v = VEREDITO_REPLANEJAR;
    else v = algum_dado ? VEREDITO_INCOMPLETO : VEREDITO_FALHA;
    if (ausentes) *ausentes = faltam ? faltam : g_new0(char *, 1);
    else g_strfreev(faltam);
    return v;
}

static const struct { const char *evidencia, *ferramenta, *parametros; } FORNECEDORES[] = {
    { "memoria_total", "sistema.recursos", "{}" }, { "memoria_total", "sistema.diagnosticar", "{}" },
    { "memoria_disponivel", "sistema.recursos", "{}" }, { "memoria_disponivel", "sistema.diagnosticar", "{}" },
    { "swap", "sistema.recursos", "{}" }, { "uptime", "sistema.recursos", "{}" }, { "carga_media", "sistema.recursos", "{}" },
    { "cpu_nucleos", "sistema.recursos", "{}" }, { "cpu_nucleos", "sistema.diagnosticar", "{}" },
    { "cpu_modelo", "sistema.diagnosticar", "{}" }, { "cpu_modelo", "sistema.recursos", "{}" },
    { "distribuicao", "sistema.diagnosticar", "{}" }, { "kernel", "sistema.diagnosticar", "{}" },
    { "lista_processos_cpu", "sistema.processos_top_cpu", "{}" },
    { "lista_processos_memoria", "sistema.processos_top_memoria", "{}" },
    { "dispositivos_rede", "sistema.dispositivos", "{\"classe\":\"rede\"}" },
    { "dispositivos_bloco", "sistema.dispositivos", "{\"classe\":\"bloco\"}" },
    { "dispositivos_pci", "sistema.dispositivos", "{\"classe\":\"pci\"}" },
    { "resultados_web", "web.pesquisar", "{}" },
    { "conteudo_web", "web.abrir", "{}" },
};

static int tentativas(const AgentePlano *p, const char *ferramenta, const char *params_norm)
{
    int n = 0;
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        if (strcmp(s->ferramenta, ferramenta) == 0 && strcmp(s->parametros_json, params_norm) == 0) n++;
    }
    return n;
}

/* acrescenta um passo de leitura pura; NULL em sucesso, senão o motivo */
static char *acrescentar_de_replano(AgentePlano *p, const char *ferramenta, const char *parametros_json,
                                     const char *evidencia, const AgenteManifesto *mf)
{
    if ((int)p->passos->len >= AGENTE_MAX_PASSOS || (int)p->passos->len >= p->orcamento_passos)
        return g_strdup_printf("orçamento de %d passos esgotado", p->orcamento_passos);
    AgenteFerramentaInfo info = { 0 };
    if (!ferramenta || !*ferramenta || !mf->info(ferramenta, &info, mf->dados))
        return g_strdup_printf("a ferramenta '%s' não existe no manifesto", ferramenta ? ferramenta : "");
    if (!info.capacidade_permitida || !info.somente_leitura || info.requer_aprovacao || nivel_risco(info.risco) != 0)
        return g_strdup_printf("'%s' não é leitura pura (risco %s%s): o replanejamento nunca acrescenta escrita, administração ou hardware a um plano em execução",
                               ferramenta, info.risco ? info.risco : "?", info.requer_aprovacao ? ", exige aprovação" : "");
    char *norm = normalizar_objeto(parametros_json);
    if (!norm)
        return g_strdup("os parâmetros sugeridos não são um objeto JSON");
    char *erro = mf->validar(ferramenta, norm, mf->dados);
    if (erro) {
        char *m = g_strdup_printf("parâmetros inválidos para '%s': %s", ferramenta, erro);
        g_free(erro);
        g_free(norm);
        return m;
    }
    if (tentativas(p, ferramenta, norm) >= AGENTE_MAX_TENTATIVAS_IGUAIS) {
        char *m = g_strdup_printf("'%s' com estes parâmetros já foi tentada %d vezes (repetir de novo não traria dado novo)", ferramenta, AGENTE_MAX_TENTATIVAS_IGUAIS);
        g_free(norm);
        return m;
    }
    const char *evs[] = { evidencia, NULL };
    char *obj = evidencia ? g_strdup_printf("obter a evidência ausente '%s' (replanejamento)", evidencia)
                          : g_strdup("passo sugerido pelo modelo no replanejamento");
    AgentePasso *s = agente_plano_acrescentar(p, NULL, ferramenta, norm, NULL, obj, evidencia ? evs : NULL,
                                               info.timeout_padrao_ms, FALSE);
    /* id único mesmo que o plano de origem tenha usado "passo-N" em outra ordem */
    while (TRUE) {
        gboolean repetido = FALSE;
        for (guint i = 0; i + 1 < p->passos->len; i++)
            if (strcmp(((AgentePasso *)g_ptr_array_index(p->passos, i))->id, s->id) == 0) repetido = TRUE;
        if (!repetido) break;
        char *novo = g_strdup_printf("%s-r", s->id);
        g_free(s->id);
        s->id = novo;
    }
    s->de_replanejamento = TRUE;
    s->somente_leitura = TRUE;
    g_free(obj);
    g_free(norm);
    return NULL;
}

int agente_replanejar(AgentePlano *p, char **ausentes, const AgenteManifesto *mf)
{
    if (p->replanejamentos >= AGENTE_MAX_REPLANEJAMENTOS) return 0;
    int acrescentados = 0;
    for (int i = 0; ausentes && ausentes[i]; i++) {
        const char *e = ausentes[i];
        /* um passo já acrescentado neste ciclo pode cobrir esta evidência também */
        gboolean coberta = FALSE;
        for (guint k = 0; k < p->passos->len && !coberta; k++) {
            AgentePasso *s = g_ptr_array_index(p->passos, k);
            if (s->estado != PASSO_AGUARDANDO) continue;
            for (gsize f = 0; f < G_N_ELEMENTS(FORNECEDORES); f++)
                if (strcmp(FORNECEDORES[f].evidencia, e) == 0 && strcmp(FORNECEDORES[f].ferramenta, s->ferramenta) == 0)
                    coberta = TRUE;
        }
        if (coberta) continue;
        gboolean feito = FALSE;
        if (g_str_has_prefix(e, "resultado:")) {
            /* evidência genérica: tenta de novo o passo que falhou, com os mesmos parâmetros */
            for (guint k = 0; k < p->passos->len && !feito; k++) {
                AgentePasso *s = g_ptr_array_index(p->passos, k);
                if (strcmp(s->ferramenta, e + 10) != 0 || s->estado == PASSO_SUCESSO) continue;
                char *erro = acrescentar_de_replano(p, s->ferramenta, s->parametros_json, e, mf);
                feito = erro == NULL;
                g_free(erro);
                break;
            }
        } else {
            for (gsize f = 0; f < G_N_ELEMENTS(FORNECEDORES) && !feito; f++) {
                if (strcmp(FORNECEDORES[f].evidencia, e) != 0) continue;
                char *erro = acrescentar_de_replano(p, FORNECEDORES[f].ferramenta, FORNECEDORES[f].parametros, e, mf);
                feito = erro == NULL;
                g_free(erro);
            }
        }
        if (feito) acrescentados++;
    }
    if (acrescentados) {
        p->replanejamentos++;
        p->versao++;
        recalcular_hash(p);
    }
    return acrescentados;
}

char *agente_replanejar_com_sugestao(AgentePlano *p, const char *ferramenta, const char *parametros_json,
                                      const AgenteManifesto *mf)
{
    if (p->replanejamentos >= AGENTE_MAX_REPLANEJAMENTOS)
        return g_strdup_printf("limite de %d ciclos de replanejamento atingido", AGENTE_MAX_REPLANEJAMENTOS);
    char *erro = acrescentar_de_replano(p, ferramenta, parametros_json, NULL, mf);
    if (erro) return erro;
    p->replanejamentos++;
    p->versao++;
    recalcular_hash(p);
    return NULL;
}

/* ------------------------------------------------------------ relatório --- */

/* leitura dos dados reais de um passo concluído, para a análise */
typedef struct {
    GPtrArray *parsers;
    gboolean tem_rec, tem_carga, tem_swap, tem_uptime;
    gint64 mem_total, mem_disp, swap_total, swap_livre, carga1, carga5, carga15, uptime, nucleos;
    const char *cpu_modelo, *distro, *kernel, *kernel_maq;
    JsonArray *top_cpu, *top_mem;
    guint n_rede, n_bloco, n_pci;
    gboolean tem_rede, tem_bloco, tem_pci;
} Analise;

static void analise_carregar(Analise *a, const AgentePlano *p)
{
    memset(a, 0, sizeof *a);
    a->parsers = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        if (s->estado != PASSO_SUCESSO || !s->dados_json) continue;
        JsonParser *jp = json_parser_new();
        g_ptr_array_add(a->parsers, jp);
        JsonNode *carga = carga_util(jp, s->dados_json);
        if (!carga) continue;
        JsonObject *d = JSON_NODE_TYPE(carga) == JSON_NODE_OBJECT ? json_node_get_object(carga) : NULL;
        gint64 v;
        if (strcmp(s->ferramenta, "sistema.recursos") == 0 || strcmp(s->ferramenta, "sistema.diagnosticar") == 0) {
            JsonObject *mem = jobj(d, "memoria_kb"), *cpu = jobj(d, "cpu");
            if (strv_tem(s->evidencias, "memoria_total") && jint(mem, "total", &v)) a->mem_total = v;
            if (strv_tem(s->evidencias, "memoria_disponivel") && jint(mem, "disponivel", &v)) { a->mem_disp = v; a->tem_rec = TRUE; }
            if (jint(cpu, "nucleos", &v) && v > 0) a->nucleos = v;
            if (jstr(cpu, "modelo")) a->cpu_modelo = jstr(cpu, "modelo");
            if (strv_tem(s->evidencias, "swap")) {
                jint(jobj(d, "swap_kb"), "total", &a->swap_total);
                jint(jobj(d, "swap_kb"), "livre", &a->swap_livre);
                a->tem_swap = TRUE;
            }
            if (strv_tem(s->evidencias, "uptime") && jint(d, "uptime_segundos", &v)) { a->uptime = v; a->tem_uptime = TRUE; }
            if (strv_tem(s->evidencias, "carga_media")) {
                JsonObject *c = jobj(d, "carga_media_centesimos");
                jint(c, "1min", &a->carga1); jint(c, "5min", &a->carga5); jint(c, "15min", &a->carga15);
                a->tem_carga = TRUE;
            }
            if (jstr(d, "distro")) a->distro = jstr(d, "distro");
            if (jstr(jobj(d, "kernel"), "release")) { a->kernel = jstr(jobj(d, "kernel"), "release"); a->kernel_maq = jstr(jobj(d, "kernel"), "machine"); }
        } else if (strcmp(s->ferramenta, "sistema.processos_top_cpu") == 0 && strv_tem(s->evidencias, "lista_processos_cpu")) {
            a->top_cpu = jarr(d, "processos");
        } else if (strcmp(s->ferramenta, "sistema.processos_top_memoria") == 0 && strv_tem(s->evidencias, "lista_processos_memoria")) {
            a->top_mem = jarr(d, "processos");
            if (!a->mem_total && jint(d, "memoria_total_kb", &v)) a->mem_total = v;
        } else if (strcmp(s->ferramenta, "sistema.dispositivos") == 0 && JSON_NODE_TYPE(carga) == JSON_NODE_ARRAY) {
            JsonArray *arr = json_node_get_array(carga);
            if (strv_tem(s->evidencias, "dispositivos_rede")) { a->tem_rede = TRUE; a->n_rede = 0; }
            if (strv_tem(s->evidencias, "dispositivos_bloco")) { a->tem_bloco = TRUE; a->n_bloco = 0; }
            if (strv_tem(s->evidencias, "dispositivos_pci")) { a->tem_pci = TRUE; a->n_pci = 0; }
            for (guint k = 0; k < json_array_get_length(arr); k++) {
                JsonNode *el = json_array_get_element(arr, k);
                if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) continue;
                const char *c = jstr(json_node_get_object(el), "classe");
                if (g_strcmp0(c, "rede") == 0) a->n_rede++;
                else if (g_strcmp0(c, "bloco") == 0) a->n_bloco++;
                else if (g_strcmp0(c, "pci") == 0) a->n_pci++;
            }
        }
    }
}

static void cent(GString *o, gint64 c)
{
    g_string_append_printf(o, "%" G_GINT64_FORMAT ",%02d", c / 100, (int)(c % 100));
}

/* nome vindo do sistema: nunca caractere de controle no relatório */
static void nome_seguro(GString *o, const char *s)
{
    for (const char *p = s ? s : "?"; *p; p++)
        g_string_append_c(o, ((unsigned char)*p < 0x20 || *p == 0x7f) ? '?' : *p);
}

static JsonObject *proc(JsonArray *a, guint i)
{
    if (!a || i >= json_array_get_length(a)) return NULL;
    JsonNode *n = json_array_get_element(a, i);
    return JSON_NODE_TYPE(n) == JSON_NODE_OBJECT ? json_node_get_object(n) : NULL;
}

static gint64 pint(JsonObject *o, const char *k)
{
    gint64 v = 0;
    jint(o, k, &v);
    return v;
}

/* Achados numéricos, com o critério dito por extenso. Devolve quantos são
 * "de atenção" (pressão medida). Cada linha diz de qual ferramenta veio. */
static int achados(const Analise *a, GString *ev, GString *alertas)
{
    int n_alerta = 0;
    if (a->tem_carga && a->nucleos > 0) {
        gint64 razao = a->carga1 / a->nucleos; /* centésimos de "carga por núcleo" */
        g_string_append(ev, "  - Carga média: ");
        cent(ev, a->carga1); g_string_append(ev, " (1 min), "); cent(ev, a->carga5); g_string_append(ev, " (5 min), ");
        cent(ev, a->carga15);
        g_string_append_printf(ev, " (15 min) em %" G_GINT64_FORMAT " núcleos — ", a->nucleos);
        cent(ev, razao);
        g_string_append(ev, " por núcleo no último minuto [sistema.recursos]\n");
        if (razao >= 100) {
            g_string_append(alertas, "a carga do último minuto passa da capacidade dos núcleos (1,00 por núcleo ou mais)");
            n_alerta++;
        } else if (razao >= 70) {
            g_string_append(alertas, "a carga do último minuto está alta (0,70 por núcleo ou mais)");
            n_alerta++;
        }
    } else if (a->tem_carga) {
        g_string_append(ev, "  - Carga média: ");
        cent(ev, a->carga1); g_string_append(ev, " (1 min), "); cent(ev, a->carga5); g_string_append(ev, " (5 min), ");
        cent(ev, a->carga15); g_string_append(ev, " (15 min) [sistema.recursos]\n");
    }
    if (a->tem_rec && a->mem_total > 0) {
        gint64 pct = (a->mem_disp * 10000 + a->mem_total / 2) / a->mem_total;
        g_string_append_printf(ev, "  - Memória: %" G_GINT64_FORMAT " MiB disponíveis de %" G_GINT64_FORMAT " MiB (",
                               a->mem_disp / 1024, a->mem_total / 1024);
        cent(ev, pct);
        g_string_append(ev, " % disponível) [sistema.recursos]\n");
        if (pct < 1000) {
            g_string_append_printf(alertas, "%spouca memória disponível (menos de 10 %%)", alertas->len ? "; " : "");
            n_alerta++;
        } else if (pct < 2500) {
            g_string_append_printf(alertas, "%smemória disponível baixa (menos de 25 %%)", alertas->len ? "; " : "");
            n_alerta++;
        }
    }
    if (a->tem_swap) {
        if (a->swap_total > 0) {
            gint64 uso = a->swap_total - a->swap_livre, pct = (uso * 10000 + a->swap_total / 2) / a->swap_total;
            g_string_append_printf(ev, "  - Swap: %" G_GINT64_FORMAT " MiB em uso de %" G_GINT64_FORMAT " MiB (",
                                   uso / 1024, a->swap_total / 1024);
            cent(ev, pct);
            g_string_append(ev, " % em uso) [sistema.recursos]\n");
            if (pct >= 5000) {
                g_string_append_printf(alertas, "%suso intenso de swap (50 %% ou mais)", alertas->len ? "; " : "");
                n_alerta++;
            }
        } else {
            g_string_append(ev, "  - Swap: não configurada [sistema.recursos]\n");
        }
    }
    JsonObject *c0 = proc(a->top_cpu, 0);
    if (c0) {
        gint64 pct = pint(c0, "cpu_pct_centesimos");
        g_string_append(ev, "  - Maior uso de CPU: ");
        nome_seguro(ev, jstr(c0, "nome"));
        g_string_append_printf(ev, " (PID %" G_GINT64_FORMAT "), ", pint(c0, "pid"));
        cent(ev, pct);
        g_string_append(ev, " % da capacidade total da máquina [sistema.processos_top_cpu]\n");
        /* "cerca de um núcleo inteiro": 80 % de (100 % / núcleos) */
        gint64 um_nucleo = a->nucleos > 0 ? 8000 / a->nucleos : 1000;
        if (pct >= um_nucleo) {
            g_string_append_printf(alertas, "%so processo ", alertas->len ? "; " : "");
            nome_seguro(alertas, jstr(c0, "nome"));
            g_string_append(alertas, " ocupa cerca de um núcleo inteiro ou mais (");
            cent(alertas, pct);
            g_string_append(alertas, " % da capacidade total)");
            n_alerta++;
        }
    }
    JsonObject *m0 = proc(a->top_mem, 0);
    if (m0) {
        gint64 pct = pint(m0, "mem_pct_centesimos");
        g_string_append(ev, "  - Maior uso de memória: ");
        nome_seguro(ev, jstr(m0, "nome"));
        g_string_append_printf(ev, " (PID %" G_GINT64_FORMAT "), %" G_GINT64_FORMAT " MiB (", pint(m0, "pid"), (pint(m0, "rss_kb") + 512) / 1024);
        cent(ev, pct);
        g_string_append(ev, " % da RAM) [sistema.processos_top_memoria]\n");
        if (pct >= 2500) {
            g_string_append_printf(alertas, "%so processo ", alertas->len ? "; " : "");
            nome_seguro(alertas, jstr(m0, "nome"));
            g_string_append(alertas, " concentra mais de 25 % da RAM (");
            cent(alertas, pct);
            g_string_append(alertas, " %)");
            n_alerta++;
        }
    }
    return n_alerta;
}

static void lista_top(GString *o, JsonArray *a, gboolean memoria, guint max)
{
    for (guint i = 0; i < max; i++) {
        JsonObject *p = proc(a, i);
        if (!p) break;
        if (!memoria && i > 0 && pint(p, "cpu_pct_centesimos") == 0) break; /* quem mediu 0,00 % não acrescenta nada à lista */
        g_string_append(o, i ? ", " : "");
        nome_seguro(o, jstr(p, "nome"));
        if (memoria) g_string_append_printf(o, " (%" G_GINT64_FORMAT " MiB)", (pint(p, "rss_kb") + 512) / 1024);
        else { g_string_append(o, " ("); cent(o, pint(p, "cpu_pct_centesimos")); g_string_append(o, " %)"); }
    }
}

static void uptime_texto(GString *o, gint64 s)
{
    g_string_append_printf(o, "%" G_GINT64_FORMAT " dias, %" G_GINT64_FORMAT " horas e %" G_GINT64_FORMAT " minutos",
                           s / 86400, (s % 86400) / 3600, (s % 3600) / 60);
}

char *agente_relatorio(const AgentePlano *p, AgenteVeredito v, char **ausentes, const char *comentario_modelo)
{
    Analise a;
    analise_carregar(&a, p);
    GString *concl = g_string_new(NULL), *ev = g_string_new(NULL), *alertas = g_string_new(NULL), *prox = g_string_new(NULL);
    const char *in = p->intencao ? p->intencao : "";
    int n_ok = 0, n_total = (int)p->passos->len;
    for (guint i = 0; i < p->passos->len; i++)
        if (((AgentePasso *)g_ptr_array_index(p->passos, i))->estado == PASSO_SUCESSO) n_ok++;
    gboolean completo = v == VEREDITO_SUCESSO;

    if (!strcmp(in, "investigar_lentidao") || !strcmp(in, "consumo_anormal") || !strcmp(in, "evidencias_sobre_malware")) {
        int n_alerta = achados(&a, ev, alertas);
        if (a.top_cpu) { g_string_append(ev, "  - Processos por CPU (os primeiros): "); lista_top(ev, a.top_cpu, FALSE, 5); g_string_append(ev, " [sistema.processos_top_cpu]\n"); }
        if (a.top_mem) { g_string_append(ev, "  - Processos por memória (os primeiros): "); lista_top(ev, a.top_mem, TRUE, 5); g_string_append(ev, " [sistema.processos_top_memoria]\n"); }
        if (a.tem_bloco) g_string_append_printf(ev, "  - Dispositivos de bloco existentes: %u [sistema.dispositivos]\n", a.n_bloco);
        if (!strcmp(in, "evidencias_sobre_malware")) {
            g_string_append(concl, "Não é possível afirmar se há ou não malware nesta máquina: não existe ferramenta de detecção nesta versão. "
                                   "Os processos abaixo são apenas os que mais usam CPU e memória agora — nenhum deles é classificado como vírus, "
                                   "e nenhum é declarado seguro.");
            if (n_alerta) g_string_append_printf(concl, " Pelo critério de consumo (e só por ele): %s.", alertas->str);
            else if (n_ok) g_string_append(concl, " Pelo critério de consumo (e só por ele), nenhum processo passou dos limites fixos usados aqui.");
            g_string_append(prox, "conferir, com ferramentas do próprio sistema, pacotes instalados e sua integridade, serviços ativos e arquivos alterados recentemente — esta versão ainda não tem ferramenta para isso");
        } else if (!strcmp(in, "consumo_anormal")) {
            if (n_alerta) g_string_append_printf(concl, "Há consumo acima dos limites fixos usados aqui: %s.", alertas->str);
            else if (completo) g_string_append(concl, "Nenhum processo passou dos limites fixos usados aqui (cerca de um núcleo inteiro de CPU, ou mais de 25 % da RAM), e carga e memória estão dentro deles, nesta medição.");
            else g_string_append(concl, "Com os dados coletados não dá para dizer se há consumo anormal.");
            g_string_append(prox, n_alerta ? "acompanhar o processo apontado por mais tempo antes de qualquer ação (nenhum processo foi alterado)"
                                           : "repetir a medição no momento em que o problema aparecer — este é só um retrato de agora");
        } else {
            if (n_alerta) g_string_append_printf(concl, "Há indícios medidos de pressão sobre o sistema agora: %s.", alertas->str);
            else if (completo) g_string_append(concl, "Os dados coletados agora não mostram pressão de CPU, memória ou swap. Isso não prova que não há lentidão: "
                                                      "a causa pode estar em algo que esta versão não mede (E/S de disco, rede, temperatura) ou em um pico que já passou.");
            else g_string_append(concl, "Com os dados coletados não dá para apontar uma causa para a lentidão.");
            g_string_append(prox, n_alerta ? "observar os processos apontados por mais tempo; nada foi encerrado nem alterado"
                                           : "repetir a coleta no momento exato da lentidão; se persistir sem indício aqui, investigar E/S de disco e rede com ferramentas do sistema");
        }
    } else if (!strcmp(in, "comparar_cpu_memoria")) {
        GString *comuns = g_string_new(NULL);
        guint n_comuns = 0, nc = a.top_cpu ? json_array_get_length(a.top_cpu) : 0, nm = a.top_mem ? json_array_get_length(a.top_mem) : 0;
        for (guint i = 0; i < nc; i++) {
            JsonObject *c = proc(a.top_cpu, i);
            for (guint j = 0; c && j < nm; j++) {
                JsonObject *m = proc(a.top_mem, j);
                if (m && pint(c, "pid") == pint(m, "pid")) {
                    g_string_append(comuns, n_comuns ? ", " : "");
                    nome_seguro(comuns, jstr(c, "nome"));
                    g_string_append_printf(comuns, " (PID %" G_GINT64_FORMAT ": ", pint(c, "pid"));
                    cent(comuns, pint(c, "cpu_pct_centesimos"));
                    g_string_append_printf(comuns, " %% de CPU, %" G_GINT64_FORMAT " MiB)", (pint(m, "rss_kb") + 512) / 1024);
                    n_comuns++;
                }
            }
        }
        if (a.top_cpu && a.top_mem) {
            if (n_comuns) g_string_append_printf(concl, "%u processo(s) aparecem nas duas listas (mesmo PID): %s.", n_comuns, comuns->str);
            else g_string_append_printf(concl, "Nenhum processo aparece nas duas listas: os %u que mais usam CPU e os %u que mais usam memória são processos diferentes.", nc, nm);
        } else {
            g_string_append(concl, "Não foi possível comparar: falta uma das duas listas.");
        }
        if (a.top_cpu) { g_string_append_printf(ev, "  - Por CPU (%u listados): ", nc); lista_top(ev, a.top_cpu, FALSE, 10); g_string_append(ev, " [sistema.processos_top_cpu]\n"); }
        if (a.top_mem) { g_string_append_printf(ev, "  - Por memória (%u listados): ", nm); lista_top(ev, a.top_mem, TRUE, 10); g_string_append(ev, " [sistema.processos_top_memoria]\n"); }
        g_string_append(prox, "se quiser, peça a análise de lentidão para ver carga, memória disponível e swap junto com estas listas");
        g_string_free(comuns, TRUE);
    } else if (!strcmp(in, "relatorio_de_recursos") || !strcmp(in, "estado_do_sistema")) {
        if (a.distro || a.kernel) {
            g_string_append(ev, "  - Sistema: ");
            nome_seguro(ev, a.distro ? a.distro : "?");
            if (a.kernel) g_string_append_printf(ev, ", kernel %s%s%s%s", a.kernel, a.kernel_maq ? " (" : "", a.kernel_maq ? a.kernel_maq : "", a.kernel_maq ? ")" : "");
            g_string_append(ev, " [sistema.diagnosticar]\n");
        }
        if (a.cpu_modelo || a.nucleos) {
            g_string_append(ev, "  - CPU: ");
            nome_seguro(ev, a.cpu_modelo ? a.cpu_modelo : "modelo não lido");
            if (a.nucleos) g_string_append_printf(ev, " — %" G_GINT64_FORMAT " núcleos", a.nucleos);
            g_string_append(ev, " [sistema.diagnosticar/sistema.recursos]\n");
        }
        int n_alerta = achados(&a, ev, alertas);
        if (a.tem_uptime) { g_string_append(ev, "  - Uptime: "); uptime_texto(ev, a.uptime); g_string_append(ev, " [sistema.recursos]\n"); }
        if (a.top_cpu) { g_string_append(ev, "  - Processos por CPU: "); lista_top(ev, a.top_cpu, FALSE, 5); g_string_append(ev, " [sistema.processos_top_cpu]\n"); }
        if (a.top_mem) { g_string_append(ev, "  - Processos por memória: "); lista_top(ev, a.top_mem, TRUE, 5); g_string_append(ev, " [sistema.processos_top_memoria]\n"); }
        if (a.distro || a.kernel) {
            g_string_append(concl, "A máquina roda ");
            nome_seguro(concl, a.distro ? a.distro : "um sistema não identificado");
            if (a.kernel) g_string_append_printf(concl, " (kernel %s)", a.kernel);
            g_string_append(concl, ". ");
        }
        if (a.tem_rec && a.mem_total > 0)
            g_string_append_printf(concl, "Há %" G_GINT64_FORMAT " MiB de RAM disponíveis de %" G_GINT64_FORMAT " MiB. ", a.mem_disp / 1024, a.mem_total / 1024);
        if (a.tem_carga) { g_string_append(concl, "Carga média de "); cent(concl, a.carga1); g_string_append(concl, " no último minuto. "); }
        if (a.tem_uptime) { g_string_append(concl, "Ligada há "); uptime_texto(concl, a.uptime); g_string_append(concl, ". "); }
        if (n_alerta) g_string_append_printf(concl, "Ponto de atenção: %s.", alertas->str);
        else if (completo) g_string_append(concl, "Nenhum dos limites fixos de atenção (carga, memória, swap) foi ultrapassado nesta medição.");
        g_string_append(prox, "nenhum necessário; para detalhar processos, peça os que mais usam CPU ou memória");
    } else if (!strcmp(in, "pesquisa_web") || !strcmp(in, "abrir_web")) {
        gboolean tem_resultados = FALSE;
        gboolean tem_cotacoes = FALSE;
        for (guint i = 0; i < p->passos->len; i++) {
            AgentePasso *s = g_ptr_array_index(p->passos, i);
            if (s->estado == PASSO_SUCESSO && (strv_tem(s->evidencias, "resultados_web") || strv_tem(s->evidencias, "conteudo_web"))) tem_resultados = TRUE;
            if (s->estado == PASSO_SUCESSO && s->dados_json && !strcmp(s->ferramenta, "web.pesquisar")) {
                JsonParser *jp = json_parser_new();
                if (json_parser_load_from_data(jp, s->dados_json, -1, NULL)) {
                    JsonNode *rn = json_parser_get_root(jp);
                    JsonObject *jo = rn && JSON_NODE_HOLDS_OBJECT(rn) ? json_node_get_object(rn) : NULL;
                    JsonArray *ca = jo && json_object_has_member(jo, "cotacoes") ? json_object_get_array_member(jo, "cotacoes") : NULL;
                    if (ca && json_array_get_length(ca) > 0) {
                        tem_cotacoes = TRUE;
                        g_string_append(concl, "Cotações estruturadas recebidas: ");
                        for (guint j = 0; j < json_array_get_length(ca); j++) {
                            JsonObject *co = json_array_get_object_element(ca, j);
                            if (!co) continue;
                            if (j) g_string_append(concl, "; ");
                            g_string_append_printf(concl, "1 %s = %.4f %s (%s)",
                                                   jstr(co, "base") ? jstr(co, "base") : "?",
                                                   json_object_get_double_member(co, "valor"),
                                                   jstr(co, "destino") ? jstr(co, "destino") : "?",
                                                   jstr(co, "data") ? jstr(co, "data") : "data não informada");
                        }
                        g_string_append(concl, ". ");
                    }
                }
                g_object_unref(jp);
            }
        }
        if (tem_resultados) {
            if (!strcmp(in, "abrir_web")) g_string_append(concl, "A página HTTPS pública foi aberta e seu texto foi extraído.");
            else if (!tem_cotacoes) g_string_append(concl, "A pesquisa web devolveu resultados estruturados e URLs para conferência humana.");
            g_string_append(prox, !strcmp(in, "abrir_web") ? "conferir o conteúdo e a data diretamente na fonte" : "abrir as fontes e comparar a data e a credibilidade antes de tomar decisões");
        } else {
            g_string_append(concl, "A pesquisa não devolveu resultados estruturados suficientes para uma conclusão.");
            g_string_append(prox, "reformular a consulta ou tentar novamente mais tarde");
        }
    } else if (!strcmp(in, "inventario_de_hardware")) {
        if (a.tem_rede) g_string_append_printf(ev, "  - Interfaces de rede: %u [sistema.dispositivos classe=rede]\n", a.n_rede);
        if (a.tem_pci) g_string_append_printf(ev, "  - Dispositivos PCI: %u [sistema.dispositivos classe=pci]\n", a.n_pci);
        if (a.tem_bloco) g_string_append_printf(ev, "  - Dispositivos de bloco: %u [sistema.dispositivos classe=bloco]\n", a.n_bloco);
        g_string_append(concl, "Inventário lido agora:");
        if (a.tem_rede) g_string_append_printf(concl, " %u interfaces de rede;", a.n_rede);
        if (a.tem_pci) g_string_append_printf(concl, " %u dispositivos PCI;", a.n_pci);
        if (a.tem_bloco) g_string_append_printf(concl, " %u dispositivos de bloco;", a.n_bloco);
        g_string_append(concl, " os nomes de cada um estão nas listas acima.");
        g_string_append(prox, "nenhum necessário");
    } else {
        g_string_append_printf(concl, "%d de %d passos do plano foram concluídos com dados; os resultados de cada um estão acima.", n_ok, n_total);
        g_string_append(prox, completo ? "nenhum necessário" : "rever os passos que falharam");
    }

    GString *o = g_string_new(NULL);
    if (v == VEREDITO_INCOMPLETO || v == VEREDITO_FALHA) {
        g_string_append_printf(o, "RESULTADO %s — a tarefa não foi concluída.\n", v == VEREDITO_FALHA ? "SEM DADOS" : "INCOMPLETO");
        g_string_append(o, "Motivo: faltam evidências exigidas e não há mais como buscá-las dentro do orçamento (");
        g_string_append_printf(o, "%d passo(s) usados de %d, %d ciclo(s) de replanejamento de %d).\n",
                               n_total, p->orcamento_passos, p->replanejamentos, AGENTE_MAX_REPLANEJAMENTOS);
        g_string_append(o, "Dados ausentes: ");
        for (int i = 0; ausentes && ausentes[i]; i++) g_string_append_printf(o, "%s%s", i ? ", " : "", ausentes[i]);
        g_string_append(o, ".\n\n");
    }
    g_string_append_printf(o, "Conclusão: %s\n", v == VEREDITO_FALHA ? "nenhum dado foi coletado; não há conclusão possível." : concl->str);
    g_string_append(o, "\nEvidências (medidas agora, por leitura direta; origem entre colchetes):\n");
    if (ev->len) g_string_append(o, ev->str);
    else {
        for (guint i = 0; i < p->passos->len; i++) {
            AgentePasso *s = g_ptr_array_index(p->passos, i);
            if (s->estado == PASSO_SUCESSO) {
                char *e = g_strjoinv(", ", s->evidencias);
                g_string_append_printf(o, "  - %s: %s\n", s->ferramenta, *e ? e : "resultado recebido");
                g_free(e);
            }
        }
        if (n_ok == 0) g_string_append(o, "  (nenhuma)\n");
    }
    g_string_append(o, "\nLimitações:\n");
    int n_lim = 0;
    for (int i = 0; p->limitacoes && p->limitacoes[i]; i++, n_lim++) g_string_append_printf(o, "  - %s\n", p->limitacoes[i]);
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        for (int k = 0; s->limitacoes && s->limitacoes[k]; k++, n_lim++)
            g_string_append_printf(o, "  - %s (%s): %s\n", s->id, s->ferramenta, s->limitacoes[k]);
    }
    for (int i = 0; ausentes && ausentes[i]; i++, n_lim++) g_string_append_printf(o, "  - evidência não obtida: %s\n", ausentes[i]);
    if (!n_lim) g_string_append(o, "  - retrato do instante da coleta; não há histórico para comparar\n");
    gboolean tem_rede = FALSE;
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        if (s->ferramenta && (!strcmp(s->ferramenta, "web.pesquisar") || !strcmp(s->ferramenta, "web.abrir"))) tem_rede = TRUE;
    }
    g_string_append(o, tem_rede ? "\nAções realizadas (sem escrita; houve acesso externo autorizado):\n"
                                : "\nAções realizadas (somente leitura):\n");
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        g_string_append_printf(o, "  - [%u/%d] %s %s — %s", i + 1, n_total, s->ferramenta, s->parametros_json, agente_passo_estado_texto(s->estado));
        if (s->estado == PASSO_SUCESSO || s->estado == PASSO_FALHA) g_string_append_printf(o, " (%" G_GINT64_FORMAT " ms)", s->duracao_ms);
        if (s->de_replanejamento) g_string_append(o, " [replanejamento]");
        if (s->erro) g_string_append_printf(o, ": %s", s->erro);
        g_string_append_c(o, '\n');
    }
    if (p->replanejamentos) g_string_append_printf(o, "  - %d ciclo(s) de replanejamento\n", p->replanejamentos);
    g_string_append(o, "\nAções não realizadas: nenhuma escrita em disco, nenhuma ação administrativa, nenhum processo encerrado ou alterado, nenhuma configuração modificada.\n");
    g_string_append_printf(o, "\nPróximo passo: %s.", (v == VEREDITO_INCOMPLETO || v == VEREDITO_FALHA) ? "tentar de novo mais tarde, ou verificar por que as ferramentas acima falharam" : prox->str);
    if (comentario_modelo && *comentario_modelo)
        g_string_append_printf(o, "\n\nLeitura do modelo (texto livre; números conferidos contra as evidências acima): %s", comentario_modelo);

    g_string_free(concl, TRUE);
    g_string_free(ev, TRUE);
    g_string_free(alertas, TRUE);
    g_string_free(prox, TRUE);
    g_ptr_array_free(a.parsers, TRUE);
    return g_string_free(o, FALSE);
}

char *agente_evidencias_texto(const AgentePlano *p)
{
    GString *o = g_string_new(NULL);
    for (guint i = 0; i < p->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(p->passos, i);
        if (s->estado != PASSO_SUCESSO || !s->apresentacao) continue;
        g_string_append_printf(o, "[%s %s]\n%s\n", s->ferramenta, s->parametros_json, s->apresentacao);
    }
    return g_string_free(o, FALSE);
}

gboolean agente_afirmacao_de_seguranca(const char *texto)
{
    static const char *const SINAIS[] = {
        " livre de virus", " livre de malware", " nao ha virus", " nao ha malware", " nao tem virus", " nao tem malware",
        " nao existe virus", " nao existe malware", " esta seguro", " esta segura", " sistema seguro", " maquina segura",
        " esta infectad", " foi infectad", " e um virus", " e malware", " sao virus", " contem virus", " contem malware",
        " nenhum virus", " nenhum malware", " sem virus", " sem malware", " esta limpo", " esta limpa", NULL };
    char *t = normalizar_pedido(texto);
    gboolean achou = tem_algum(t, SINAIS);
    g_free(t);
    return achou;
}

gboolean agente_conclusao_positiva_sem_base(const char *texto)
{
    static const char *const SINAIS[] = {
        " sem problema", " nenhum problema", " nao ha problema", " funcionando normalmente", " funciona normalmente",
        " operando normalmente", " esta tudo bem", " tudo certo", " tudo normal", " esta saudavel", " nada de errado",
        " nao ha nada errado", " em perfeito estado", " funcionando bem", " funcionando corretamente", NULL };
    char *t = normalizar_pedido(texto);
    gboolean achou = tem_algum(t, SINAIS);
    g_free(t);
    return achou;
}
