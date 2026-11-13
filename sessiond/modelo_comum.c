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
