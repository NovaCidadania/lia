/* Lia — implementação de gui/protocolo.h. Ver docs/protocolo.md. */
#include "protocolo.h"
#include <string.h>

static const struct { const char *nome; LiaMsgTipo tipo; } TABELA_TIPOS[] = {
    { "solicitar", LIA_MSG_SOLICITAR },
    { "aprovar",   LIA_MSG_APROVAR },
    { "cancelar",  LIA_MSG_CANCELAR },
    { "assinar",   LIA_MSG_ASSINAR },
    { "perguntar", LIA_MSG_PERGUNTAR },
    { "plano",     LIA_MSG_PLANO },
    { "progresso", LIA_MSG_PROGRESSO },
    { "saida",     LIA_MSG_SAIDA },
    { "resultado", LIA_MSG_RESULTADO },
    { "auditoria", LIA_MSG_AUDITORIA },
    { "erro",      LIA_MSG_ERRO },
    { "proposta",  LIA_MSG_PROPOSTA },
    { "resposta",  LIA_MSG_RESPOSTA },
};
#define N_TIPOS (sizeof(TABELA_TIPOS) / sizeof(TABELA_TIPOS[0]))

LiaMsgTipo lia_msg_tipo_de_string(const char *s)
{
    if (!s)
        return LIA_MSG_DESCONHECIDO;
    for (size_t i = 0; i < N_TIPOS; i++)
        if (strcmp(s, TABELA_TIPOS[i].nome) == 0)
            return TABELA_TIPOS[i].tipo;
    return LIA_MSG_DESCONHECIDO;
}

const char *lia_msg_tipo_para_string(LiaMsgTipo t)
{
    for (size_t i = 0; i < N_TIPOS; i++)
        if (TABELA_TIPOS[i].tipo == t)
            return TABELA_TIPOS[i].nome;
    return "desconhecido";
}

const char *lia_msg_erro_texto(LiaMsgErro e)
{
    switch (e) {
    case LIA_MSG_OK: return "ok";
    case LIA_MSG_ERRO_JSON_INVALIDO: return "linha não é JSON válido";
    case LIA_MSG_ERRO_ENVELOPE_INVALIDO: return "envelope incompleto (protocolo/tipo/id/ts/dados)";
    case LIA_MSG_ERRO_PROTOCOLO_INCOMPATIVEL: return "protocolo_incompativel";
    case LIA_MSG_ERRO_TIPO_DESCONHECIDO: return "tipo de mensagem desconhecido";
    default: return "erro desconhecido";
    }
}

LiaMsgErro lia_msg_parsear(const char *linha, LiaMsg *out)
{
    memset(out, 0, sizeof(*out));
    if (!linha || !*linha)
        return LIA_MSG_ERRO_JSON_INVALIDO;

    JsonParser *parser = json_parser_new();
    GError *erro = NULL;
    if (!json_parser_load_from_data(parser, linha, -1, &erro)) {
        if (erro) g_error_free(erro);
        g_object_unref(parser);
        return LIA_MSG_ERRO_JSON_INVALIDO;
    }

    JsonNode *raiz = json_parser_get_root(parser);
    if (!raiz || JSON_NODE_TYPE(raiz) != JSON_NODE_OBJECT) {
        g_object_unref(parser);
        return LIA_MSG_ERRO_ENVELOPE_INVALIDO;
    }
    JsonObject *obj = json_node_get_object(raiz);

    if (!json_object_has_member(obj, "protocolo") ||
        !json_object_has_member(obj, "tipo") ||
        !json_object_has_member(obj, "id") ||
        !json_object_has_member(obj, "ts")) {
        g_object_unref(parser);
        return LIA_MSG_ERRO_ENVELOPE_INVALIDO;
    }

    JsonNode *n_proto = json_object_get_member(obj, "protocolo");
    JsonNode *n_tipo = json_object_get_member(obj, "tipo");
    JsonNode *n_id = json_object_get_member(obj, "id");
    JsonNode *n_ts = json_object_get_member(obj, "ts");
    if (JSON_NODE_TYPE(n_proto) != JSON_NODE_VALUE || JSON_NODE_TYPE(n_tipo) != JSON_NODE_VALUE ||
        JSON_NODE_TYPE(n_id) != JSON_NODE_VALUE || JSON_NODE_TYPE(n_ts) != JSON_NODE_VALUE) {
        g_object_unref(parser);
        return LIA_MSG_ERRO_ENVELOPE_INVALIDO;
    }

    const char *proto = json_node_get_string(n_proto);
    if (!proto || strcmp(proto, LIA_PROTOCOLO_VERSAO) != 0) {
        g_object_unref(parser);
        return LIA_MSG_ERRO_PROTOCOLO_INCOMPATIVEL;
    }

    /* "dados" é opcional na linha (algumas mensagens não têm payload),
     * mas se presente precisa ser objeto. */
    JsonObject *dados = NULL;
    if (json_object_has_member(obj, "dados")) {
        JsonNode *n_dados = json_object_get_member(obj, "dados");
        if (JSON_NODE_TYPE(n_dados) != JSON_NODE_OBJECT) {
            g_object_unref(parser);
            return LIA_MSG_ERRO_ENVELOPE_INVALIDO;
        }
        dados = json_node_get_object(n_dados);
    }

    LiaMsgTipo tipo = lia_msg_tipo_de_string(json_node_get_string(n_tipo));
    if (tipo == LIA_MSG_DESCONHECIDO) {
        g_object_unref(parser);
        return LIA_MSG_ERRO_TIPO_DESCONHECIDO;
    }

    char *em_resposta_a = NULL;
    if (json_object_has_member(obj, "em_resposta_a")) {
        JsonNode *n_er = json_object_get_member(obj, "em_resposta_a");
        if (JSON_NODE_TYPE(n_er) == JSON_NODE_VALUE && json_node_get_value_type(n_er) == G_TYPE_STRING)
            em_resposta_a = g_strdup(json_node_get_string(n_er));
    }

    out->tipo = tipo;
    out->id = g_strdup(json_node_get_string(n_id));
    out->em_resposta_a = em_resposta_a;
    out->ts = g_strdup(json_node_get_string(n_ts));
    out->dados = dados;
    out->_parser = parser; /* mantém obj/dados vivos */
    return LIA_MSG_OK;
}

void lia_msg_liberar(LiaMsg *m)
{
    if (!m)
        return;
    g_free(m->id);
    g_free(m->em_resposta_a);
    g_free(m->ts);
    if (m->_parser)
        g_object_unref(m->_parser);
    memset(m, 0, sizeof(*m));
}

char *lia_ts_agora(void)
{
    GDateTime *agora = g_date_time_new_now_utc();
    char *ts = g_date_time_format(agora, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(agora);
    return ts;
}

char *lia_novo_id(const char *prefixo)
{
    static guint64 contador = 0;
    return g_strdup_printf("%s-%" G_GINT64_FORMAT "-%" G_GUINT64_FORMAT,
                            prefixo, g_get_monotonic_time(), ++contador);
}

char *lia_msg_montar(LiaMsgTipo tipo, const char *id, const char *em_resposta_a, JsonBuilder *dados)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "protocolo");
    json_builder_add_string_value(b, LIA_PROTOCOLO_VERSAO);
    json_builder_set_member_name(b, "tipo");
    json_builder_add_string_value(b, lia_msg_tipo_para_string(tipo));
    json_builder_set_member_name(b, "id");
    json_builder_add_string_value(b, id);
    json_builder_set_member_name(b, "em_resposta_a");
    if (em_resposta_a)
        json_builder_add_string_value(b, em_resposta_a);
    else
        json_builder_add_null_value(b);
    char *ts = lia_ts_agora();
    json_builder_set_member_name(b, "ts");
    json_builder_add_string_value(b, ts);
    g_free(ts);
    json_builder_set_member_name(b, "dados");
    if (dados) {
        JsonNode *n_dados = json_builder_get_root(dados);
        json_builder_add_value(b, n_dados);
    } else {
        json_builder_begin_object(b);
        json_builder_end_object(b);
    }
    json_builder_end_object(b);

    JsonNode *raiz = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, raiz);
    json_generator_set_pretty(gen, FALSE);
    gsize tamanho = 0;
    char *texto = json_generator_to_data(gen, &tamanho);
    json_node_unref(raiz);
    g_object_unref(gen);
    g_object_unref(b);

    char *linha = g_strdup_printf("%s\n", texto);
    g_free(texto);
    return linha;
}

/* ------------------------------- sessão -------------------------------- */

void lia_sessao_iniciar(LiaSessao *s)
{
    memset(s, 0, sizeof(*s));
    s->modo_leitura = TRUE;
    s->ids_vistos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->execucoes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

void lia_sessao_liberar(LiaSessao *s)
{
    if (!s)
        return;
    if (s->ids_vistos) g_hash_table_destroy(s->ids_vistos);
    if (s->execucoes) g_hash_table_destroy(s->execucoes);
    memset(s, 0, sizeof(*s));
}

void lia_sessao_conectar(LiaSessao *s) { s->conectado = TRUE; }

void lia_sessao_desconectar(LiaSessao *s)
{
    /* Deliberado: NÃO limpa ids_vistos/execucoes. docs/protocolo.md §4 garante
     * que o daemon emite 'resultado' mesmo se o cliente desconectar antes;
     * do lado do cliente, perder o estado de uma execução pendente só porque
     * a conexão caiu escureceria essa garantia na UI (voltaria a mostrar
     * "nada pendente" quando na verdade ainda não se sabe o resultado). */
    s->conectado = FALSE;
}

LiaSessaoErro lia_sessao_receber(LiaSessao *s, const LiaMsg *m)
{
    if (m->em_resposta_a && !g_hash_table_contains(s->ids_vistos, m->em_resposta_a))
        return LIA_SESSAO_ERRO_RESPOSTA_DESCONHECIDA;

    if (m->dados && (m->tipo == LIA_MSG_PROGRESSO || m->tipo == LIA_MSG_SAIDA || m->tipo == LIA_MSG_RESULTADO)) {
        JsonNode *n_exec = json_object_has_member(m->dados, "execucao_id")
            ? json_object_get_member(m->dados, "execucao_id") : NULL;
        const char *exec_id = (n_exec && JSON_NODE_TYPE(n_exec) == JSON_NODE_VALUE)
            ? json_node_get_string(n_exec) : NULL;
        if (exec_id) {
            gpointer achado = g_hash_table_lookup(s->execucoes, exec_id);
            LiaExecEstado estado_atual = achado ? GPOINTER_TO_INT(achado) : LIA_EXEC_EM_ANDAMENTO;
            if (achado && estado_atual == LIA_EXEC_CONCLUIDA)
                return LIA_SESSAO_ERRO_EXECUCAO_JA_CONCLUIDA;
            LiaExecEstado novo = (m->tipo == LIA_MSG_RESULTADO) ? LIA_EXEC_CONCLUIDA : LIA_EXEC_EM_ANDAMENTO;
            g_hash_table_replace(s->execucoes, g_strdup(exec_id), GINT_TO_POINTER(novo));
        }
    }

    g_hash_table_add(s->ids_vistos, g_strdup(m->id));
    return LIA_SESSAO_OK;
}

LiaEnvioErro lia_sessao_validar_envio(const LiaSessao *s, LiaMsgTipo tipo,
                                      const char * const *canais, size_t n_canais)
{
    if (tipo == LIA_MSG_CANCELAR) {
        /* permitido mesmo em modo_leitura: cancelar revoga uma ação já em
         * andamento, não inicia um efeito novo. */
        GHashTableIter it;
        gpointer k, v;
        gboolean ha_pendente = FALSE;
        g_hash_table_iter_init(&it, s->execucoes);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            if (GPOINTER_TO_INT(v) == LIA_EXEC_EM_ANDAMENTO) { ha_pendente = TRUE; break; }
        }
        return ha_pendente ? LIA_ENVIO_OK : LIA_ENVIO_RECUSADO_NADA_PARA_CANCELAR;
    }

    if (tipo == LIA_MSG_ASSINAR) {
        for (size_t i = 0; i < n_canais; i++) {
            if (strcmp(canais[i], "auditoria") != 0 && strcmp(canais[i], "execucao") != 0)
                return LIA_ENVIO_RECUSADO_CANAL_DESCONHECIDO;
        }
        return LIA_ENVIO_OK;
    }

    /* 'aprovar' é o único tipo que a GUI nunca envia nesta etapa: as seis
     * ferramentas de leitura do executor real (docs/manifesto.md) têm
     * risco=nenhum e requer_aprovacao=false — nenhuma delas jamais chega
     * ao estado "aguardando aprovação", então a GUI não precisa (e não
     * deve) aprovar nada sozinha. 'solicitar' É permitido: o daemon
     * continua sendo a autoridade (rejeita ferramenta_desconhecida,
     * capacidade_negada, parametro_invalido); a GUI só evita pedir o que
     * sabe, de antemão, que teria efeito. */
    if (s->modo_leitura && tipo == LIA_MSG_APROVAR)
        return LIA_ENVIO_RECUSADO_SOMENTE_LEITURA;

    return LIA_ENVIO_OK;
}
