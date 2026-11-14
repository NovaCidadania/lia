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
#define OLLAMA_LIMITE_CABECALHOS_BYTES (8 * 1024)

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
    cfg->timeout_ms = 60000; /* CPU-only: medido ~34s (8,7s prompt-eval + 25s geração,
                              * llama3.2:3b, prompt real da Lia) numa máquina com uso
                              * normal — 15s (valor antigo) estourava rotineiramente
                              * antes do primeiro token chegar. */
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

gboolean ollama_host_e_loopback(const char *host)
{
    return host && (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0);
}

/* "127.0.0.1" já é um literal loopback, sem precisar resolver nada.
 * "localhost" depende de NSS/`/etc/hosts` — resolve de verdade e confirma
 * que TODO endereço retornado é loopback (IPv4 127.0.0.0/8 ou IPv6 ::1)
 * antes de aceitar, porque a resolução desse nome pode, em tese, ter sido
 * alterada no sistema (docs/THREAT-MODEL.md §5.2). Nunca conecta nada —
 * só resolve o nome. */
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

char *ollama_config_validar(const OllamaConfig *cfg)
{
    if (!cfg)
        return g_strdup("configuração ausente");
    if (!ollama_host_e_loopback(cfg->host))
        return g_strdup_printf("host inválido: '%s' (só 127.0.0.1/localhost é aceito)", cfg->host);
    char *erro_resolucao = NULL;
    if (!resolve_e_confirma_loopback(cfg->host, &erro_resolucao))
        return erro_resolucao ? erro_resolucao : g_strdup("host não confirmado como loopback");
    g_free(erro_resolucao); /* NULL em sucesso; g_free(NULL) é inofensivo */
    if (cfg->porta < OLLAMA_PORTA_MIN || cfg->porta > OLLAMA_PORTA_MAX)
        return g_strdup_printf("porta inválida: %d (esperado %d..%d)", cfg->porta, OLLAMA_PORTA_MIN, OLLAMA_PORTA_MAX);
    if (cfg->timeout_ms < OLLAMA_TIMEOUT_MS_MIN || cfg->timeout_ms > OLLAMA_TIMEOUT_MS_MAX)
        return g_strdup_printf("timeout_ms inválido: %d (esperado %d..%d)",
                                cfg->timeout_ms, OLLAMA_TIMEOUT_MS_MIN, OLLAMA_TIMEOUT_MS_MAX);
    if (!cfg->modelo || !*cfg->modelo)
        return g_strdup("nome do modelo vazio");
    if (strlen(cfg->modelo) > OLLAMA_MODELO_TAM_MAX)
        return g_strdup_printf("nome do modelo acima do limite (%d bytes)", OLLAMA_MODELO_TAM_MAX);
    return NULL;
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
        /* Content-Length conhecido: nunca consome além dele — bytes
         * excedentes (lixo após o corpo declarado) ficam de fora, nunca
         * entram no fluxo NDJSON. Desconhecido (-1): comportamento
         * anterior, consome tudo que chegou (o fim real é o EOF). */
        gsize alvo = disponivel;
        if (req->content_length >= 0) {
            gsize limite_corpo = (gsize)req->content_length;
            alvo = limite_corpo < disponivel ? limite_corpo : disponivel;
        }
        if (req->pos_consumida < alvo) {
            g_string_append_len(req->corpo_decodificado, base + req->pos_consumida, alvo - req->pos_consumida);
            req->pos_consumida = alvo;
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
        /* nada suportado depois dos dígitos hex: nenhuma extensão de
         * chunk ("<hex>;nome=valor") é reconhecida — melhor rejeitar
         * explicitamente do que ignorar silenciosamente algo que não
         * entendemos (RFC 7230 §4.1.1 permite extensões, mas o Ollama
         * nunca as envia; aceitar mais do que o necessário é só
         * superfície de ataque extra). */
        if (*fim_parse != '\0') {
            finalizar(req, OLLAMA_ERRO_RESPOSTA_INVALIDA);
            return FALSE;
        }
        /* um único chunk maior que o limite inteiro de resposta nunca
         * pode ser válido — falha já, em vez de esperar indefinidamente
         * por bytes que (se chegarem) só seriam descartados depois. */
        if ((guint64)tam_chunk > (guint64)req->cfg->limite_resposta_bytes) {
            finalizar(req, OLLAMA_ERRO_LIMITE_EXCEDIDO);
            return FALSE;
        }

        gsize offset_dados = tam_linha_tam + 1; /* depois do '\n' do tamanho */
        if (tam_restante < offset_dados + (gsize)tam_chunk + 2)
            return TRUE; /* chunk ainda não chegou inteiro (dados + \r\n final) */

        /* confere rigorosamente o CRLF depois dos dados do chunk — nunca
         * avança por cima de dois bytes não verificados. */
        if (restante[offset_dados + (gsize)tam_chunk] != '\r' ||
            restante[offset_dados + (gsize)tam_chunk + 1] != '\n') {
            finalizar(req, OLLAMA_ERRO_RESPOSTA_INVALIDA);
            return FALSE;
        }

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
    if (!fim) {
        /* nunca espera indefinidamente por um terminador de cabeçalhos
         * que pode nunca chegar — limite fixo, independente do limite
         * geral de resposta (que só é checado depois de decodificado). */
        if (req->bruto->len > OLLAMA_LIMITE_CABECALHOS_BYTES) {
            finalizar(req, OLLAMA_ERRO_RESPOSTA_INVALIDA);
            return FALSE;
        }
        return TRUE; /* ainda não chegaram todos os cabeçalhos */
    }

    gsize tam_cabecalhos = (gsize)(fim - req->bruto->str);
    if (tam_cabecalhos > OLLAMA_LIMITE_CABECALHOS_BYTES) {
        finalizar(req, OLLAMA_ERRO_RESPOSTA_INVALIDA);
        return FALSE;
    }
    char *cabecalhos = g_strndup(req->bruto->str, tam_cabecalhos);

    /* status: "HTTP/1.1 200 OK\r\n..." */
    int status = 0;
    if (sscanf(cabecalhos, "HTTP/%*d.%*d %d", &status) != 1) {
        g_free(cabecalhos);
        finalizar(req, OLLAMA_ERRO_RESPOSTA_INVALIDA);
        return FALSE;
    }
    req->status_http = status;

    /* nomes de cabeçalho HTTP são case-insensitive (RFC 7230 §3.2) —
     * nunca checar só uma ou duas variações de maiúsculas/minúsculas.
     * Percorre linha a linha (pula a linha de status, índice 0) e compara
     * o nome de cada cabeçalho com g_ascii_strcasecmp. */
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
            req->content_length = cl >= 0 ? cl : -1; /* negativo malformado: trata como "desconhecido" */
        }
    }
    g_strfreev(linhas);

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

    char *erro_cfg = ollama_config_validar(cfg);
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

void ollama_cancelar(OllamaRequisicao *req)
{
    if (!req || req->finalizado)
        return;
    if (req->cancelavel)
        g_cancellable_cancel(req->cancelavel);
    /* a operação assíncrona pendente (connect ou read) vai retornar com
     * G_IO_ERROR_CANCELLED e finalizar() será chamada a partir de lá. */
}
