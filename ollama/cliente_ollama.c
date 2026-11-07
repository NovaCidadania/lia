/* Lia — implementação de ollama/cliente_ollama.h. Ver o cabeçalho para o
 * contrato. Usa só GIO (GSocketClient/GSocketConnection) — a mesma
 * biblioteca que sessiond/sessiond.c já usa para o socket Unix do
 * protocolo, nunca libcurl.
 */
#include "cliente_ollama.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define OLLAMA_CHUNK_LEITURA 4096

typedef enum {
    FASE_CONECTANDO,
    FASE_LENDO_CABECALHOS,
    FASE_LENDO_CORPO,
    FASE_ENCERRADA,
} Fase;

struct OllamaRequisicao {
    OllamaConfig *cfg;        /* referência; não possui */
    OllamaConcluidoCb concluido_cb;
    gpointer ctx;

    GSocketClient *cliente_socket;
    GSocketConnection *conexao;
    GCancellable *cancelavel;
    GInputStream *entrada;

    GString *bruto;             /* todos os bytes já lidos (cabeçalhos + corpo bruto) */
    gsize pos_cabecalho_fim;    /* índice em 'bruto' logo após "\r\n\r\n", 0 se ainda não achado */
    gsize pos_consumida;        /* até onde o corpo (pós-cabeçalho) já foi decodificado */

    GString *corpo_decodificado; /* corpo HTTP já des-chunked, ainda não processado em linhas NDJSON */
    GString *texto_completo;     /* concatenação dos fragmentos "response" */

    gboolean usa_chunked;
    glong content_length;        /* -1 se desconhecido/não aplicável */
    int status_http;
    gboolean viu_done;

    Fase fase;
    gboolean finalizado;
    guint timer_inatividade;
    char chunk_leitura[OLLAMA_CHUNK_LEITURA];
};

const char *ollama_erro_texto(OllamaErro e)
{
    switch (e) {
    case OLLAMA_OK: return "ok";
    case OLLAMA_ERRO_ENDPOINT_INVALIDO: return "endpoint inválido (só 127.0.0.1/localhost é aceito)";
    case OLLAMA_ERRO_CONEXAO: return "não foi possível conectar ao Ollama (indisponível?)";
    case OLLAMA_ERRO_TIMEOUT: return "tempo limite excedido";
    case OLLAMA_ERRO_LIMITE_EXCEDIDO: return "resposta excedeu o limite de tamanho configurado";
    case OLLAMA_ERRO_RESPOSTA_INVALIDA: return "resposta HTTP/JSON malformada ou incompleta";
    case OLLAMA_ERRO_CANCELADO: return "cancelado";
    case OLLAMA_ERRO_HTTP: return "Ollama respondeu com erro HTTP";
    default: return "erro desconhecido";
    }
}

OllamaConfig *ollama_config_novo(const char *host_ou_null, int porta, const char *modelo)
{
    OllamaConfig *cfg = g_new0(OllamaConfig, 1);
    cfg->host = g_strdup(host_ou_null && *host_ou_null ? host_ou_null : "127.0.0.1");
    cfg->porta = porta > 0 ? porta : 11434;
    cfg->modelo = g_strdup(modelo && *modelo ? modelo : "llama3");
    cfg->timeout_ms = 15000;
    cfg->limite_resposta_bytes = 1024 * 1024; /* 1 MiB — suficiente para uma resposta de chat */
    return cfg;
}

void ollama_config_liberar(OllamaConfig *cfg)
{
    if (!cfg) return;
    g_free(cfg->host);
    g_free(cfg->modelo);
    g_free(cfg);
}

static gboolean host_e_loopback(const char *host)
{
    return host && (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0);
}

static void requisicao_liberar_recursos(OllamaRequisicao *req)
{
    if (req->timer_inatividade) { g_source_remove(req->timer_inatividade); req->timer_inatividade = 0; }
    if (req->conexao) { g_io_stream_close(G_IO_STREAM(req->conexao), NULL, NULL); g_object_unref(req->conexao); req->conexao = NULL; }
    if (req->cliente_socket) { g_object_unref(req->cliente_socket); req->cliente_socket = NULL; }
    if (req->cancelavel) { g_object_unref(req->cancelavel); req->cancelavel = NULL; }
    if (req->bruto) { g_string_free(req->bruto, TRUE); req->bruto = NULL; }
    if (req->corpo_decodificado) { g_string_free(req->corpo_decodificado, TRUE); req->corpo_decodificado = NULL; }
}

static void finalizar(OllamaRequisicao *req, OllamaErro erro)
{
    if (req->finalizado)
        return;
    req->finalizado = TRUE;
    req->fase = FASE_ENCERRADA;
    const char *texto = (erro == OLLAMA_OK) ? req->texto_completo->str : NULL;
    req->concluido_cb(req->ctx, erro, texto);
    requisicao_liberar_recursos(req);
    g_string_free(req->texto_completo, TRUE);
    g_free(req);
}

static gboolean ao_timeout_inatividade(gpointer dados)
{
    OllamaRequisicao *req = dados;
    req->timer_inatividade = 0;
    finalizar(req, OLLAMA_ERRO_TIMEOUT);
    return G_SOURCE_REMOVE;
}

static void rearmar_timeout(OllamaRequisicao *req)
{
    if (req->timer_inatividade)
        g_source_remove(req->timer_inatividade);
    req->timer_inatividade = g_timeout_add(req->cfg->timeout_ms, ao_timeout_inatividade, req);
}

/* Processa o que já estiver disponível em corpo_decodificado: cada linha
 * completa é um objeto JSON (docs Ollama: /api/generate streaming). Linha
 * malformada -> RESPOSTA_INVALIDA (falha segura: nunca ignora silenciosamente). */
static gboolean processar_linhas_ndjson(OllamaRequisicao *req)
{
    for (;;) {
        char *nl = memchr(req->corpo_decodificado->str, '\n', req->corpo_decodificado->len);
        if (!nl)
            return TRUE;
        gsize tam = (gsize)(nl - req->corpo_decodificado->str);
        char *linha = g_strndup(req->corpo_decodificado->str, tam);
        g_string_erase(req->corpo_decodificado, 0, (gssize)(tam + 1));

        if (tam == 0) { g_free(linha); continue; } /* linha vazia: ignora, não é erro */

        JsonParser *p = json_parser_new();
        gboolean ok = json_parser_load_from_data(p, linha, -1, NULL);
        g_free(linha);
        if (!ok) { g_object_unref(p); finalizar(req, OLLAMA_ERRO_RESPOSTA_INVALIDA); return FALSE; }
        JsonNode *raiz = json_parser_get_root(p);
        if (!raiz || JSON_NODE_TYPE(raiz) != JSON_NODE_OBJECT) {
            g_object_unref(p); finalizar(req, OLLAMA_ERRO_RESPOSTA_INVALIDA); return FALSE;
        }
        JsonObject *obj = json_node_get_object(raiz);
        if (json_object_has_member(obj, "response")) {
            JsonNode *n = json_object_get_member(obj, "response");
            if (JSON_NODE_TYPE(n) == JSON_NODE_VALUE && json_node_get_value_type(n) == G_TYPE_STRING)
                g_string_append(req->texto_completo, json_node_get_string(n));
        }
        gboolean done = json_object_has_member(obj, "done") &&
                        JSON_NODE_TYPE(json_object_get_member(obj, "done")) == JSON_NODE_VALUE &&
                        json_object_get_boolean_member(obj, "done");
        g_object_unref(p);

        if ((gsize)req->texto_completo->len > req->cfg->limite_resposta_bytes) {
            finalizar(req, OLLAMA_ERRO_LIMITE_EXCEDIDO);
            return FALSE;
        }
        if (done) {
            req->viu_done = TRUE;
            finalizar(req, OLLAMA_OK);
            return FALSE;
        }
    }
}

/* Decodifica o que puder de req->bruto[pos_consumida:] para
 * corpo_decodificado, respeitando chunked ou Content-Length. Retorna
 * FALSE se já finalizou (erro) — chamador deve parar de usar req. */
static gboolean decodificar_corpo_disponivel(OllamaRequisicao *req)
{
    const char *base = req->bruto->str + req->pos_cabecalho_fim;
    gsize disponivel = req->bruto->len - req->pos_cabecalho_fim;

    if (!req->usa_chunked) {
        /* Content-Length conhecido ou desconhecido: tudo que chegou após
         * os cabeçalhos é corpo direto. */
        if (req->pos_consumida < disponivel) {
            g_string_append_len(req->corpo_decodificado, base + req->pos_consumida, disponivel - req->pos_consumida);
            req->pos_consumida = disponivel;
        }
        return processar_linhas_ndjson(req);
    }

    /* chunked: <hex>\r\n<dados><hex seguinte>\r\n ... 0\r\n\r\n */
    for (;;) {
        const char *restante = base + req->pos_consumida;
        gsize tam_restante = disponivel - req->pos_consumida;
        char *fim_linha = memchr(restante, '\n', tam_restante);
        if (!fim_linha)
            return TRUE; /* linha de tamanho ainda incompleta; espera mais bytes */
        gsize tam_linha_tam = (gsize)(fim_linha - restante);
        char buf_tam[32];
        gsize copiar = tam_linha_tam < sizeof(buf_tam) - 1 ? tam_linha_tam : sizeof(buf_tam) - 1;
        memcpy(buf_tam, restante, copiar);
        buf_tam[copiar] = 0;
        /* remove um possível '\r' final */
        if (copiar > 0 && buf_tam[copiar - 1] == '\r') buf_tam[copiar - 1] = 0;

        char *fim_parse = NULL;
        long tam_chunk = strtol(buf_tam, &fim_parse, 16);
        if (fim_parse == buf_tam || tam_chunk < 0) {
            finalizar(req, OLLAMA_ERRO_RESPOSTA_INVALIDA);
            return FALSE;
        }

        gsize offset_dados = tam_linha_tam + 1; /* depois do '\n' do tamanho */
        if (tam_restante < offset_dados + (gsize)tam_chunk + 2)
            return TRUE; /* chunk ainda não chegou inteiro (dados + \r\n final) */

        if (tam_chunk == 0) {
            /* chunk terminador: fim do corpo */
            req->pos_consumida += offset_dados + 2;
            return processar_linhas_ndjson(req);
        }

        g_string_append_len(req->corpo_decodificado, restante + offset_dados, (gsize)tam_chunk);
        req->pos_consumida += offset_dados + (gsize)tam_chunk + 2;

        if ((gsize)req->corpo_decodificado->len > req->cfg->limite_resposta_bytes) {
            finalizar(req, OLLAMA_ERRO_LIMITE_EXCEDIDO);
            return FALSE;
        }
    }
}

static gboolean tentar_parsear_cabecalhos(OllamaRequisicao *req)
{
    const char *fim = g_strstr_len(req->bruto->str, (gssize)req->bruto->len, "\r\n\r\n");
    if (!fim)
        return TRUE; /* ainda não chegaram todos os cabeçalhos */

    gsize tam_cabecalhos = (gsize)(fim - req->bruto->str);
    char *cabecalhos = g_strndup(req->bruto->str, tam_cabecalhos);

    /* status: "HTTP/1.1 200 OK\r\n..." */
    int status = 0;
    if (sscanf(cabecalhos, "HTTP/%*d.%*d %d", &status) != 1) {
        g_free(cabecalhos);
        finalizar(req, OLLAMA_ERRO_RESPOSTA_INVALIDA);
        return FALSE;
    }
    req->status_http = status;

    req->usa_chunked = (g_strstr_len(cabecalhos, -1, "Transfer-Encoding: chunked") != NULL) ||
                        (g_strstr_len(cabecalhos, -1, "transfer-encoding: chunked") != NULL);
    req->content_length = -1;
    const char *cl = g_strstr_len(cabecalhos, -1, "Content-Length:");
    if (!cl) cl = g_strstr_len(cabecalhos, -1, "content-length:");
    if (cl) req->content_length = strtol(cl + strlen("Content-Length:"), NULL, 10);

    g_free(cabecalhos);
    req->pos_cabecalho_fim = tam_cabecalhos + 4;
    req->pos_consumida = 0;
    req->fase = FASE_LENDO_CORPO;

    if (status != 200) {
        finalizar(req, OLLAMA_ERRO_HTTP);
        return FALSE;
    }
    return TRUE;
}

static void comecar_leitura(OllamaRequisicao *req);

static void ao_ler(GObject *origem, GAsyncResult *res, gpointer dados)
{
    OllamaRequisicao *req = dados;
    GError *erro = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(origem), res, &erro);
    if (req->finalizado) { if (erro) g_error_free(erro); return; }

    if (n < 0) {
        gboolean cancelado = erro && g_error_matches(erro, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        if (erro) g_error_free(erro);
        finalizar(req, cancelado ? OLLAMA_ERRO_CANCELADO : OLLAMA_ERRO_CONEXAO);
        return;
    }
    if (n == 0) {
        /* EOF: só é sucesso se já vimos 'done' (não deveria chegar aqui
         * nesse caso — finalizar já teria sido chamado); EOF antes disso
         * é um streaming interrompido/truncado. */
        finalizar(req, req->viu_done ? OLLAMA_OK : OLLAMA_ERRO_RESPOSTA_INVALIDA);
        return;
    }

    rearmar_timeout(req);
    g_string_append_len(req->bruto, req->chunk_leitura, n);

    if ((gsize)req->bruto->len > req->cfg->limite_resposta_bytes * 4) {
        /* cabeçalhos nunca deveriam chegar a isto; proteção extra antes
         * mesmo de decodificar corpo. */
        finalizar(req, OLLAMA_ERRO_LIMITE_EXCEDIDO);
        return;
    }

    if (req->fase == FASE_LENDO_CABECALHOS) {
        if (!tentar_parsear_cabecalhos(req))
            return; /* já finalizou (erro) */
        if (req->fase == FASE_LENDO_CABECALHOS) {
            comecar_leitura(req); /* cabeçalhos ainda incompletos */
            return;
        }
    }

    if (!decodificar_corpo_disponivel(req))
        return; /* já finalizou */

    comecar_leitura(req);
}

static void comecar_leitura(OllamaRequisicao *req)
{
    g_input_stream_read_async(req->entrada, req->chunk_leitura, sizeof(req->chunk_leitura),
                               G_PRIORITY_DEFAULT, req->cancelavel, ao_ler, req);
}

static void ao_conectar(GObject *origem, GAsyncResult *res, gpointer dados)
{
    OllamaRequisicao *req = dados;
    GError *erro = NULL;
    GSocketConnection *conexao = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(origem), res, &erro);
    if (req->finalizado) { if (conexao) g_object_unref(conexao); if (erro) g_error_free(erro); return; }

    if (!conexao) {
        gboolean cancelado = erro && g_error_matches(erro, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        if (erro) g_error_free(erro);
        finalizar(req, cancelado ? OLLAMA_ERRO_CANCELADO : OLLAMA_ERRO_CONEXAO);
        return;
    }
    req->conexao = conexao;
    req->entrada = g_io_stream_get_input_stream(G_IO_STREAM(conexao));
    GOutputStream *saida = g_io_stream_get_output_stream(G_IO_STREAM(conexao));

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "model");
    json_builder_add_string_value(b, req->cfg->modelo);
    json_builder_set_member_name(b, "prompt");
    const char *prompt_guardado = g_object_get_data(G_OBJECT(req->cliente_socket), "lia-prompt");
    json_builder_add_string_value(b, prompt_guardado ? prompt_guardado : "");
    json_builder_set_member_name(b, "stream");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_end_object(b);
    JsonNode *raiz = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, raiz);
    char *corpo = json_generator_to_data(gen, NULL);
    json_node_unref(raiz);
    g_object_unref(gen);
    g_object_unref(b);

    char *requisicao = g_strdup_printf(
        "POST /api/generate HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        req->cfg->host, req->cfg->porta, strlen(corpo), corpo);
    g_free(corpo);

    GError *erro_escrita = NULL;
    gboolean ok = g_output_stream_write_all(saida, requisicao, strlen(requisicao), NULL, req->cancelavel, &erro_escrita);
    g_free(requisicao);
    if (!ok) {
        gboolean cancelado = erro_escrita && g_error_matches(erro_escrita, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        if (erro_escrita) g_error_free(erro_escrita);
        finalizar(req, cancelado ? OLLAMA_ERRO_CANCELADO : OLLAMA_ERRO_CONEXAO);
        return;
    }

    req->fase = FASE_LENDO_CABECALHOS;
    rearmar_timeout(req);
    comecar_leitura(req);
}

static gboolean ao_idle_endpoint_invalido(gpointer dados)
{
    OllamaRequisicao *req = dados;
    req->concluido_cb(req->ctx, OLLAMA_ERRO_ENDPOINT_INVALIDO, NULL);
    /* nesta fase só 'texto_completo' chegou a ser alocado (ver
     * ollama_gerar: a checagem de host roda antes de qualquer outro
     * recurso) — mas é um GString, não basta g_free: precisa liberar o
     * buffer interno dele também (g_string_free), senão vaza. */
    g_string_free(req->texto_completo, TRUE);
    g_free(req);
    return G_SOURCE_REMOVE;
}

OllamaRequisicao *ollama_gerar(OllamaConfig *cfg, const char *prompt,
                                OllamaConcluidoCb concluido_cb, gpointer ctx)
{
    OllamaRequisicao *req = g_new0(OllamaRequisicao, 1);
    req->cfg = cfg;
    req->concluido_cb = concluido_cb;
    req->ctx = ctx;
    req->texto_completo = g_string_new(NULL);
    req->content_length = -1;

    if (!host_e_loopback(cfg->host)) {
        g_idle_add(ao_idle_endpoint_invalido, req);
        return NULL;
    }

    req->bruto = g_string_new(NULL);
    req->corpo_decodificado = g_string_new(NULL);
    req->cancelavel = g_cancellable_new();
    req->cliente_socket = g_socket_client_new();
    g_socket_client_set_timeout(req->cliente_socket, (guint)((cfg->timeout_ms + 999) / 1000));
    g_object_set_data_full(G_OBJECT(req->cliente_socket), "lia-prompt", g_strdup(prompt), g_free);
    req->fase = FASE_CONECTANDO;

    g_socket_client_connect_to_host_async(req->cliente_socket, cfg->host, (guint16)cfg->porta,
                                           req->cancelavel, ao_conectar, req);
    return req;
}

void ollama_cancelar(OllamaRequisicao *req)
{
    if (!req || req->finalizado)
        return;
    if (req->cancelavel)
        g_cancellable_cancel(req->cancelavel);
    /* a operação assíncrona pendente (connect ou read) vai retornar com
     * G_IO_ERROR_CANCELLED e finalizar() será chamada a partir de lá. */
}
