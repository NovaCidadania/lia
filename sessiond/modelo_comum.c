#include "modelo.h"
#include <json-glib/json-glib.h>
#include <string.h>

void modelo_resposta_liberar(ModeloResposta *r)
{
    if (!r)
        return;
    g_free(r->resposta_texto);
    g_free(r->ferramenta_proposta);
    g_free(r->parametros_json);
    g_free(r->justificativa);
    g_free(r->erro_codigo);
    g_free(r->erro_mensagem);
    g_free(r->tipo);
}

/* Procura o primeiro objeto JSON top-level em 'texto' (varredura simples
 * de chaves balanceadas, ignorando chaves dentro de strings). Retorna um
 * trecho alocado (g_free) ou NULL se não achar nada bem formado. */
static char *extrair_bloco_json(const char *texto)
{
    const char *inicio = strchr(texto, '{');
    if (!inicio)
        return NULL;
    int profundidade = 0;
    gboolean em_string = FALSE, escapado = FALSE;
    for (const char *p = inicio; *p; p++) {
        if (em_string) {
            if (escapado) escapado = FALSE;
            else if (*p == '\\') escapado = TRUE;
            else if (*p == '"') em_string = FALSE;
            continue;
        }
        if (*p == '"') { em_string = TRUE; continue; }
        if (*p == '{') profundidade++;
        else if (*p == '}') {
            profundidade--;
            if (profundidade == 0)
                return g_strndup(inicio, (gsize)(p - inicio + 1));
        }
    }
    return NULL; /* chaves não balanceadas: nada confiável para extrair */
}

void modelo_preencher_proposta_de_texto(ModeloResposta *r, const char *texto_completo)
{
    r->resposta_texto = g_strdup(texto_completo);

    char *bloco = extrair_bloco_json(texto_completo);
    if (!bloco)
        return;

    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, bloco, -1, NULL)) {
        JsonNode *raiz = json_parser_get_root(p);
        if (raiz && JSON_NODE_TYPE(raiz) == JSON_NODE_OBJECT) {
            JsonObject *obj = json_node_get_object(raiz);
            if (json_object_has_member(obj, "ferramenta")) {
                JsonNode *nf = json_object_get_member(obj, "ferramenta");
                if (JSON_NODE_TYPE(nf) == JSON_NODE_VALUE && json_node_get_value_type(nf) == G_TYPE_STRING) {
                    r->ferramenta_proposta = g_strdup(json_node_get_string(nf));
                    if (json_object_has_member(obj, "parametros")) {
                        JsonNode *np = json_object_get_member(obj, "parametros");
                        if (JSON_NODE_TYPE(np) == JSON_NODE_OBJECT) {
                            JsonGenerator *gen = json_generator_new();
                            json_generator_set_root(gen, np);
                            r->parametros_json = json_generator_to_data(gen, NULL);
                            g_object_unref(gen);
                        }
                    }
                    if (!r->parametros_json) r->parametros_json = g_strdup("{}");
                    if (json_object_has_member(obj, "justificativa")) {
                        JsonNode *nj = json_object_get_member(obj, "justificativa");
                        if (JSON_NODE_TYPE(nj) == JSON_NODE_VALUE && json_node_get_value_type(nj) == G_TYPE_STRING)
                            r->justificativa = g_strdup(json_node_get_string(nj));
                    }
                }
            }
        }
    }
    g_object_unref(p);
    g_free(bloco);
}

static const char *membro_string(JsonObject *obj, const char *chave)
{
    if (!json_object_has_member(obj, chave)) return NULL;
    JsonNode *n = json_object_get_member(obj, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
    return json_node_get_string(n);
}

void modelo_preencher_proposta_estruturada(ModeloResposta *r, const char *texto_completo)
{
    /* o texto INTEIRO precisa ser um objeto JSON (só espaço em volta é
     * tolerado) — nada de procurar um bloco no meio de prosa aqui. */
    const char *ini = texto_completo ? texto_completo : "";
    while (*ini == ' ' || *ini == '\n' || *ini == '\t' || *ini == '\r') ini++;
    JsonParser *p = json_parser_new();
    gboolean ok = *ini == '{' && json_parser_load_from_data(p, ini, -1, NULL) &&
                  json_parser_get_root(p) && JSON_NODE_TYPE(json_parser_get_root(p)) == JSON_NODE_OBJECT;
    JsonObject *obj = ok ? json_node_get_object(json_parser_get_root(p)) : NULL;
    /* precisa ter cara do formato pedido: ao menos "resposta" (texto) ou "tipo" */
    if (!obj || (!membro_string(obj, "resposta") && !membro_string(obj, "tipo"))) {
        g_object_unref(p);
        modelo_preencher_proposta_de_texto(r, texto_completo ? texto_completo : "");
        r->estruturada = FALSE;
        return;
    }
    r->estruturada = TRUE;
    const char *resposta = membro_string(obj, "resposta");
    r->resposta_texto = g_strdup(resposta ? resposta : "");
    g_strstrip(r->resposta_texto);
    const char *tipo = membro_string(obj, "tipo");
    r->tipo = tipo ? g_strdup(tipo) : NULL;
    const char *ferramenta = membro_string(obj, "ferramenta");
    if (ferramenta && *ferramenta && strcmp(ferramenta, "nenhuma") != 0 && strcmp(ferramenta, "null") != 0) {
        r->ferramenta_proposta = g_strdup(ferramenta);
        if (json_object_has_member(obj, "parametros")) {
            JsonNode *np = json_object_get_member(obj, "parametros");
            JsonGenerator *gen = json_generator_new();
            json_generator_set_root(gen, np);
            /* qualquer coisa que não seja objeto segue como veio (ex.:
             * "[]", "\"x\""): quem decide que isso é inválido é o roteador,
             * nunca este parser trocando silenciosamente por {}. */
            r->parametros_json = json_generator_to_data(gen, NULL);
            g_object_unref(gen);
        } else {
            r->parametros_json = g_strdup("{}");
        }
        const char *just = membro_string(obj, "justificativa");
        if (just && *just) r->justificativa = g_strdup(just);
    }
    g_object_unref(p);
}
