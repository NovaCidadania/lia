/* Lia — implementação de ollama/cliente_llama_cpp.h. Ver o cabeçalho para
 * o contrato. Mesma base de transporte HTTP (GIO, chunked/Content-Length,
 * timeout de inatividade, cancelamento) de ollama/cliente_ollama.c — só a
 * decodificação do corpo é diferente (SSE em vez de NDJSON), porque é
 * isso que o llama-server fala.
 */
#include "cliente_llama_cpp.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LLAMACPP_CHUNK_LEITURA 4096
#define LLAMACPP_LIMITE_CABECALHOS_BYTES (8 * 1024)

typedef enum {
    FASE_CONECTANDO,
    FASE_LENDO_CABECALHOS,
    FASE_LENDO_CORPO,
    FASE_ENCERRADA,
} Fase;

struct LlamaCppRequisicao {
    LlamaCppConfig *cfg;        /* referência; não possui */
    LlamaCppConcluidoCb concluido_cb;
    gpointer ctx;

    GSocketClient *cliente_socket;
    GSocketConnection *conexao;
    GCancellable *cancelavel;
    GInputStream *entrada;

    GString *bruto;
    gsize pos_cabecalho_fim;
    gsize pos_consumida;

    GString *corpo_decodificado;
    GString *texto_completo;

    gboolean usa_chunked;
    glong content_length;
    int status_http;
    gboolean viu_done;

    Fase fase;
    gboolean finalizado;
    guint timer_inatividade;
    char chunk_leitura[LLAMACPP_CHUNK_LEITURA];
};

const char *llama_cpp_erro_texto(LlamaCppErro e)
{
    switch (e) {
    case LLAMACPP_OK: return "ok";
    case LLAMACPP_ERRO_ENDPOINT_INVALIDO: return "endpoint inválido (só 127.0.0.1/localhost é aceito)";
    case LLAMACPP_ERRO_CONEXAO: return "não foi possível conectar ao llama-server (indisponível?)";
    case LLAMACPP_ERRO_TIMEOUT: return "tempo limite excedido";
    case LLAMACPP_ERRO_LIMITE_EXCEDIDO: return "resposta excedeu o limite de tamanho configurado";
    case LLAMACPP_ERRO_RESPOSTA_INVALIDA: return "resposta HTTP/SSE malformada ou incompleta";
    case LLAMACPP_ERRO_CANCELADO: return "cancelado";
    case LLAMACPP_ERRO_HTTP: return "llama-server respondeu com erro";
    default: return "erro desconhecido";
    }
}

LlamaCppConfig *llama_cpp_config_novo(const char *host_ou_null, int porta)
{
    LlamaCppConfig *cfg = g_new0(LlamaCppConfig, 1);
    cfg->host = g_strdup(host_ou_null && *host_ou_null ? host_ou_null : "127.0.0.1");
    cfg->porta = porta > 0 ? porta : 8080; /* padrão do próprio llama-server */
    cfg->timeout_ms = 30000; /* inferência em CPU é mais lenta — mais folga que o padrão do Ollama */
    cfg->limite_resposta_bytes = 1024 * 1024;
    return cfg;
}

void llama_cpp_config_liberar(LlamaCppConfig *cfg)
{
    if (!cfg) return;
    g_free(cfg->host);
    g_free(cfg);
}

gboolean llama_cpp_host_e_loopback(const char *host)
{
    return host && (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0);
}

static gboolean resolve_e_confirma_loopback(const char *host, char **erro_saida)
{
    if (strcmp(host, "127.0.0.1") == 0)
        return TRUE;

    GResolver *resolver = g_resolver_get_default();
    GError *erro = NULL;
    GList *enderecos = g_resolver_lookup_by_name(resolver, host, NULL, &erro);
    g_object_unref(resolver);
    if (!enderecos) {
        if (erro_saida)
            *erro_saida = g_strdup_printf("não foi possível resolver '%s': %s",
                                           host, erro ? erro->message : "erro desconhecido");
        if (erro) g_error_free(erro);
        return FALSE;
    }
    gboolean todos_loopback = TRUE;
    for (GList *it = enderecos; it; it = it->next) {
        if (!g_inet_address_get_is_loopback(G_INET_ADDRESS(it->data))) {
            todos_loopback = FALSE;
            break;
        }
    }
    g_resolver_free_addresses(enderecos);
    if (!todos_loopback && erro_saida)
        *erro_saida = g_strdup_printf("'%s' resolve para um endereço que não é loopback — recusado", host);
    return todos_loopback;
}

char *llama_cpp_config_validar(const LlamaCppConfig *cfg)
{
    if (!cfg)
        return g_strdup("configuração ausente");
    if (!llama_cpp_host_e_loopback(cfg->host))
        return g_strdup_printf("host inválido: '%s' (só 127.0.0.1/localhost é aceito)", cfg->host);
    char *erro_resolucao = NULL;
    if (!resolve_e_confirma_loopback(cfg->host, &erro_resolucao))
        return erro_resolucao ? erro_resolucao : g_strdup("host não confirmado como loopback");
    g_free(erro_resolucao);
    if (cfg->porta < LLAMACPP_PORTA_MIN || cfg->porta > LLAMACPP_PORTA_MAX)
        return g_strdup_printf("porta inválida: %d (esperado %d..%d)", cfg->porta, LLAMACPP_PORTA_MIN, LLAMACPP_PORTA_MAX);
    if (cfg->timeout_ms < LLAMACPP_TIMEOUT_MS_MIN || cfg->timeout_ms > LLAMACPP_TIMEOUT_MS_MAX)
        return g_strdup_printf("timeout_ms inválido: %d (esperado %d..%d)",
                                cfg->timeout_ms, LLAMACPP_TIMEOUT_MS_MIN, LLAMACPP_TIMEOUT_MS_MAX);
    return NULL;
}

static void requisicao_liberar_recursos(LlamaCppRequisicao *req)
{
    if (req->timer_inatividade) { g_source_remove(req->timer_inatividade); req->timer_inatividade = 0; }
    if (req->conexao) { g_io_stream_close(G_IO_STREAM(req->conexao), NULL, NULL); g_object_unref(req->conexao); req->conexao = NULL; }
    if (req->cliente_socket) { g_object_unref(req->cliente_socket); req->cliente_socket = NULL; }
    if (req->cancelavel) { g_object_unref(req->cancelavel); req->cancelavel = NULL; }
    if (req->bruto) { g_string_free(req->bruto, TRUE); req->bruto = NULL; }
    if (req->corpo_decodificado) { g_string_free(req->corpo_decodificado, TRUE); req->corpo_decodificado = NULL; }
}

static void finalizar(LlamaCppRequisicao *req, LlamaCppErro erro)
{
    if (req->finalizado)
        return;
    req->finalizado = TRUE;
    req->fase = FASE_ENCERRADA;
    const char *texto = (erro == LLAMACPP_OK) ? req->texto_completo->str : NULL;
    req->concluido_cb(req->ctx, erro, texto);
    requisicao_liberar_recursos(req);
    g_string_free(req->texto_completo, TRUE);
    g_free(req);
}

static gboolean ao_timeout_inatividade(gpointer dados)
{
    LlamaCppRequisicao *req = dados;
    req->timer_inatividade = 0;
    finalizar(req, LLAMACPP_ERRO_TIMEOUT);
    return G_SOURCE_REMOVE;
}

static void rearmar_timeout(LlamaCppRequisicao *req)
{
    if (req->timer_inatividade)
        g_source_remove(req->timer_inatividade);
    req->timer_inatividade = g_timeout_add(req->cfg->timeout_ms, ao_timeout_inatividade, req);
}

/* Processa eventos SSE completos já disponíveis em corpo_decodificado:
 * cada evento termina em uma linha vazia ("\n\n"). Dentro de um evento,
 * linhas começando com ":" são comentários/keep-alive (ignoradas); uma
 * linha "data: <json>" traz o payload. Qualquer outra coisa (formato não
 * documentado pelo llama-server) é RESPOSTA_INVALIDA — nunca ignorada
 * silenciosamente, mesma política de processar_linhas_ndjson() no
 * cliente Ollama. */
static gboolean processar_eventos_sse(LlamaCppRequisicao *req)
{
    for (;;) {
        char *fim = g_strstr_len(req->corpo_decodificado->str, (gssize)req->corpo_decodificado->len, "\n\n");
        if (!fim)
            return TRUE;
        gsize tam_evento = (gsize)(fim - req->corpo_decodificado->str);
        char *bloco = g_strndup(req->corpo_decodificado->str, tam_evento);
        g_string_erase(req->corpo_decodificado, 0, (gssize)(tam_evento + 2));

        char **linhas = g_strsplit(bloco, "\n", 0);
        g_free(bloco);
        gboolean teve_dados = FALSE;

        for (int i = 0; linhas[i] != NULL; i++) {
            char *linha = linhas[i];
            gsize tam = strlen(linha);
            if (tam > 0 && linha[tam - 1] == '\r') linha[--tam] = '\0';
            if (tam == 0) continue;
            if (linha[0] == ':') continue; /* comentário/keep-alive SSE */
            if (!g_str_has_prefix(linha, "data:")) {
                g_strfreev(linhas);
                finalizar(req, LLAMACPP_ERRO_RESPOSTA_INVALIDA);
                return FALSE;
            }
            teve_dados = TRUE;
            const char *json_txt = linha + 5;
            while (*json_txt == ' ') json_txt++;

            JsonParser *p = json_parser_new();
            gboolean ok = json_parser_load_from_data(p, json_txt, -1, NULL);
            if (!ok) {
                g_object_unref(p);
                g_strfreev(linhas);
                finalizar(req, LLAMACPP_ERRO_RESPOSTA_INVALIDA);
                return FALSE;
            }
            JsonNode *raiz = json_parser_get_root(p);
            if (!raiz || JSON_NODE_TYPE(raiz) != JSON_NODE_OBJECT) {
                g_object_unref(p);
                g_strfreev(linhas);
                finalizar(req, LLAMACPP_ERRO_RESPOSTA_INVALIDA);
                return FALSE;
            }
            JsonObject *obj = json_node_get_object(raiz);

            if (json_object_has_member(obj, "error")) {
                g_object_unref(p);
                g_strfreev(linhas);
                finalizar(req, LLAMACPP_ERRO_HTTP);
                return FALSE;
            }
            if (json_object_has_member(obj, "content")) {
                JsonNode *n = json_object_get_member(obj, "content");
                if (JSON_NODE_TYPE(n) == JSON_NODE_VALUE && json_node_get_value_type(n) == G_TYPE_STRING)
                    g_string_append(req->texto_completo, json_node_get_string(n));
            }
            gboolean parar = json_object_has_member(obj, "stop") &&
                             JSON_NODE_TYPE(json_object_get_member(obj, "stop")) == JSON_NODE_VALUE &&
                             json_object_get_boolean_member(obj, "stop");
            g_object_unref(p);

            if ((gsize)req->texto_completo->len > req->cfg->limite_resposta_bytes) {
                g_strfreev(linhas);
                finalizar(req, LLAMACPP_ERRO_LIMITE_EXCEDIDO);
                return FALSE;
            }
            if (parar) {
                req->viu_done = TRUE;
                g_strfreev(linhas);
                finalizar(req, LLAMACPP_OK);
                return FALSE;
            }
        }
        g_strfreev(linhas);
        (void)teve_dados; /* evento sem nenhuma linha "data:" (só comentário): segue para o próximo */
    }
}

static gboolean decodificar_corpo_disponivel(LlamaCppRequisicao *req)
{
    const char *base = req->bruto->str + req->pos_cabecalho_fim;
    gsize disponivel = req->bruto->len - req->pos_cabecalho_fim;

    if (!req->usa_chunked) {
        gsize alvo = disponivel;
        if (req->content_length >= 0) {
            gsize limite_corpo = (gsize)req->content_length;
            alvo = limite_corpo < disponivel ? limite_corpo : disponivel;
        }
        if (req->pos_consumida < alvo) {
            g_string_append_len(req->corpo_decodificado, base + req->pos_consumida, alvo - req->pos_consumida);
            req->pos_consumida = alvo;
        }
        return processar_eventos_sse(req);
    }

    for (;;) {
        const char *restante = base + req->pos_consumida;
        gsize tam_restante = disponivel - req->pos_consumida;
        char *fim_linha = memchr(restante, '\n', tam_restante);
        if (!fim_linha)
            return TRUE;
        gsize tam_linha_tam = (gsize)(fim_linha - restante);
        char buf_tam[32];
        gsize copiar = tam_linha_tam < sizeof(buf_tam) - 1 ? tam_linha_tam : sizeof(buf_tam) - 1;
        memcpy(buf_tam, restante, copiar);
        buf_tam[copiar] = 0;
        if (copiar > 0 && buf_tam[copiar - 1] == '\r') buf_tam[copiar - 1] = 0;

        char *fim_parse = NULL;
        long tam_chunk = strtol(buf_tam, &fim_parse, 16);
        if (fim_parse == buf_tam || tam_chunk < 0) {
            finalizar(req, LLAMACPP_ERRO_RESPOSTA_INVALIDA);
            return FALSE;
        }
        if (*fim_parse != '\0') {
            finalizar(req, LLAMACPP_ERRO_RESPOSTA_INVALIDA);
            return FALSE;
        }
        if ((guint64)tam_chunk > (guint64)req->cfg->limite_resposta_bytes) {
            finalizar(req, LLAMACPP_ERRO_LIMITE_EXCEDIDO);
            return FALSE;
        }

        gsize offset_dados = tam_linha_tam + 1;
        if (tam_restante < offset_dados + (gsize)tam_chunk + 2)
            return TRUE;

        if (restante[offset_dados + (gsize)tam_chunk] != '\r' ||
            restante[offset_dados + (gsize)tam_chunk + 1] != '\n') {
            finalizar(req, LLAMACPP_ERRO_RESPOSTA_INVALIDA);
            return FALSE;
        }

        if (tam_chunk == 0) {
            req->pos_consumida += offset_dados + 2;
            return processar_eventos_sse(req);
        }

        g_string_append_len(req->corpo_decodificado, restante + offset_dados, (gsize)tam_chunk);
        req->pos_consumida += offset_dados + (gsize)tam_chunk + 2;

        if ((gsize)req->corpo_decodificado->len > req->cfg->limite_resposta_bytes) {
            finalizar(req, LLAMACPP_ERRO_LIMITE_EXCEDIDO);
            return FALSE;
        }
    }
}

static gboolean tentar_parsear_cabecalhos(LlamaCppRequisicao *req)
{
    const char *fim = g_strstr_len(req->bruto->str, (gssize)req->bruto->len, "\r\n\r\n");
    if (!fim) {
        if (req->bruto->len > LLAMACPP_LIMITE_CABECALHOS_BYTES) {
            finalizar(req, LLAMACPP_ERRO_RESPOSTA_INVALIDA);
            return FALSE;
        }
        return TRUE;
    }

    gsize tam_cabecalhos = (gsize)(fim - req->bruto->str);
    if (tam_cabecalhos > LLAMACPP_LIMITE_CABECALHOS_BYTES) {
        finalizar(req, LLAMACPP_ERRO_RESPOSTA_INVALIDA);
        return FALSE;
    }
    char *cabecalhos = g_strndup(req->bruto->str, tam_cabecalhos);

    int status = 0;
    if (sscanf(cabecalhos, "HTTP/%*d.%*d %d", &status) != 1) {
        g_free(cabecalhos);
        finalizar(req, LLAMACPP_ERRO_RESPOSTA_INVALIDA);
        return FALSE;
    }
    req->status_http = status;

    req->usa_chunked = FALSE;
    req->content_length = -1;
    char **linhas = g_strsplit(cabecalhos, "\r\n", 0);
    for (int i = 1; linhas[i] != NULL; i++) {
        if (!*linhas[i]) continue;
        char *dois_pontos = strchr(linhas[i], ':');
        if (!dois_pontos) continue;
        *dois_pontos = '\0';
        const char *nome = linhas[i];
        const char *valor = dois_pontos + 1;
        while (*valor == ' ') valor++;
        if (g_ascii_strcasecmp(nome, "Transfer-Encoding") == 0) {
            char *valor_min = g_ascii_strdown(valor, -1);
            if (strstr(valor_min, "chunked")) req->usa_chunked = TRUE;
            g_free(valor_min);
        } else if (g_ascii_strcasecmp(nome, "Content-Length") == 0) {
            long cl = strtol(valor, NULL, 10);
            req->content_length = cl >= 0 ? cl : -1;
        }
    }
    g_strfreev(linhas);

    g_free(cabecalhos);
    req->pos_cabecalho_fim = tam_cabecalhos + 4;
    req->pos_consumida = 0;
    req->fase = FASE_LENDO_CORPO;

    if (status != 200) {
        finalizar(req, LLAMACPP_ERRO_HTTP);
        return FALSE;
    }
    return TRUE;
}

static void comecar_leitura(LlamaCppRequisicao *req);

static void ao_ler(GObject *origem, GAsyncResult *res, gpointer dados)
{
    LlamaCppRequisicao *req = dados;
    GError *erro = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(origem), res, &erro);
    if (req->finalizado) { if (erro) g_error_free(erro); return; }

    if (n < 0) {
        gboolean cancelado = erro && g_error_matches(erro, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        if (erro) g_error_free(erro);
        finalizar(req, cancelado ? LLAMACPP_ERRO_CANCELADO : LLAMACPP_ERRO_CONEXAO);
        return;
    }
    if (n == 0) {
        finalizar(req, req->viu_done ? LLAMACPP_OK : LLAMACPP_ERRO_RESPOSTA_INVALIDA);
        return;
    }

    rearmar_timeout(req);
    g_string_append_len(req->bruto, req->chunk_leitura, n);

    if ((gsize)req->bruto->len > req->cfg->limite_resposta_bytes * 4) {
        finalizar(req, LLAMACPP_ERRO_LIMITE_EXCEDIDO);
        return;
    }

    if (req->fase == FASE_LENDO_CABECALHOS) {
        if (!tentar_parsear_cabecalhos(req))
            return;
        if (req->fase == FASE_LENDO_CABECALHOS) {
            comecar_leitura(req);
            return;
        }
    }

    if (!decodificar_corpo_disponivel(req))
        return;

    comecar_leitura(req);
}

static void comecar_leitura(LlamaCppRequisicao *req)
{
    g_input_stream_read_async(req->entrada, req->chunk_leitura, sizeof(req->chunk_leitura),
                               G_PRIORITY_DEFAULT, req->cancelavel, ao_ler, req);
}

static void ao_conectar(GObject *origem, GAsyncResult *res, gpointer dados)
{
    LlamaCppRequisicao *req = dados;
    GError *erro = NULL;
    GSocketConnection *conexao = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(origem), res, &erro);
    if (req->finalizado) { if (conexao) g_object_unref(conexao); if (erro) g_error_free(erro); return; }

    if (!conexao) {
        gboolean cancelado = erro && g_error_matches(erro, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        if (erro) g_error_free(erro);
        finalizar(req, cancelado ? LLAMACPP_ERRO_CANCELADO : LLAMACPP_ERRO_CONEXAO);
        return;
    }
    req->conexao = conexao;
    req->entrada = g_io_stream_get_input_stream(G_IO_STREAM(conexao));
    GOutputStream *saida = g_io_stream_get_output_stream(G_IO_STREAM(conexao));

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
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
        "POST /completion HTTP/1.1\r\n"
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
        finalizar(req, cancelado ? LLAMACPP_ERRO_CANCELADO : LLAMACPP_ERRO_CONEXAO);
        return;
    }

    req->fase = FASE_LENDO_CABECALHOS;
    rearmar_timeout(req);
    comecar_leitura(req);
}

static gboolean ao_idle_endpoint_invalido(gpointer dados)
{
    LlamaCppRequisicao *req = dados;
    req->concluido_cb(req->ctx, LLAMACPP_ERRO_ENDPOINT_INVALIDO, NULL);
    g_string_free(req->texto_completo, TRUE);
    g_free(req);
    return G_SOURCE_REMOVE;
}

LlamaCppRequisicao *llama_cpp_gerar(LlamaCppConfig *cfg, const char *prompt,
                                     LlamaCppConcluidoCb concluido_cb, gpointer ctx)
{
    LlamaCppRequisicao *req = g_new0(LlamaCppRequisicao, 1);
    req->cfg = cfg;
    req->concluido_cb = concluido_cb;
    req->ctx = ctx;
    req->texto_completo = g_string_new(NULL);
    req->content_length = -1;

    char *erro_cfg = llama_cpp_config_validar(cfg);
    if (erro_cfg) {
        g_free(erro_cfg);
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

void llama_cpp_cancelar(LlamaCppRequisicao *req)
{
    if (!req || req->finalizado)
        return;
    if (req->cancelavel)
        g_cancellable_cancel(req->cancelavel);
}
