/* lia-sessiond — backend de modelo real sobre um llama-server local
 * (llama.cpp), via ollama/cliente_llama_cpp.c. Espelha modelo_ollama.c —
 * mesma política de configuração e de extração de proposta de
 * ferramenta (sessiond/modelo_comum.c:modelo_preencher_proposta_de_texto).
 *
 * Configuração só por variável de ambiente: LIA_LLAMACPP_HOST (padrão
 * "127.0.0.1", só aceita "127.0.0.1"/"localhost"), LIA_LLAMACPP_PORTA
 * (padrão 8080 — o padrão do próprio llama-server; ajuste para a porta
 * real do seu `llama-server --port ...`), LIA_LLAMACPP_TIMEOUT_MS
 * (padrão 30000 — inferência em CPU é mais lenta que GPU).
 *
 * Sem variável de modelo: o llama-server serve um único .gguf por
 * processo (passado a ele via `-m`), não por requisição.
 */
#include "modelo.h"
#include "../ollama/cliente_llama_cpp.h"
#include <gio/gio.h>
#include <stdlib.h>

typedef struct {
    char *pergunta_id;
    ModeloConcluidoCb concluido_cb;
    gpointer ctx;
    LlamaCppRequisicao *req;
} TarefaLlamaCpp;

static LlamaCppConfig *g_cfg;
static GHashTable *g_em_andamento; /* pergunta_id -> TarefaLlamaCpp* */

static LlamaCppConfig *obter_config(void)
{
    if (g_cfg)
        return g_cfg;
    const char *host = g_getenv("LIA_LLAMACPP_HOST");
    const char *porta_s = g_getenv("LIA_LLAMACPP_PORTA");
    const char *timeout_s = g_getenv("LIA_LLAMACPP_TIMEOUT_MS");
    g_cfg = llama_cpp_config_novo(host, porta_s ? atoi(porta_s) : 0);
    if (timeout_s) g_cfg->timeout_ms = atoi(timeout_s);
    return g_cfg;
}

static void ao_concluir_llama_cpp(gpointer ctx, LlamaCppErro erro, const char *texto_completo)
{
    TarefaLlamaCpp *t = ctx;
    ModeloResposta r = { 0 };

    if (erro == LLAMACPP_OK) {
        r.sucesso = TRUE;
        modelo_preencher_proposta_de_texto(&r, texto_completo);
    } else {
        r.sucesso = FALSE;
        r.erro_codigo = g_strdup(
            erro == LLAMACPP_ERRO_CANCELADO ? "cancelado" :
            erro == LLAMACPP_ERRO_TIMEOUT ? "limite_excedido" :
            erro == LLAMACPP_ERRO_LIMITE_EXCEDIDO ? "limite_excedido" :
            erro == LLAMACPP_ERRO_CONEXAO ? "modelo_indisponivel" :
            "modelo_resposta_invalida");
        r.erro_mensagem = g_strdup(llama_cpp_erro_texto(erro));
    }

    t->concluido_cb(t->ctx, &r);
    modelo_resposta_liberar(&r);

    if (g_em_andamento)
        g_hash_table_remove(g_em_andamento, t->pergunta_id);
    g_free(t->pergunta_id);
    g_free(t);
}

static void llama_cpp_perguntar(const ModeloBackend *self, const char *pergunta_id,
                                 const char *prompt_montado, ModeloConcluidoCb concluido_cb, gpointer ctx)
{
    (void)self;
    if (!g_em_andamento)
        g_em_andamento = g_hash_table_new(g_str_hash, g_str_equal);

    TarefaLlamaCpp *t = g_new0(TarefaLlamaCpp, 1);
    t->pergunta_id = g_strdup(pergunta_id);
    t->concluido_cb = concluido_cb;
    t->ctx = ctx;
    g_hash_table_insert(g_em_andamento, t->pergunta_id, t);
    t->req = llama_cpp_gerar(obter_config(), prompt_montado, ao_concluir_llama_cpp, t);
}

static void llama_cpp_cancelar_pergunta(const ModeloBackend *self, const char *pergunta_id)
{
    (void)self;
    if (!g_em_andamento)
        return;
    TarefaLlamaCpp *t = g_hash_table_lookup(g_em_andamento, pergunta_id);
    if (t && t->req)
        llama_cpp_cancelar(t->req);
}

static gboolean llama_cpp_checar_disponivel(const ModeloBackend *self, char **detalhe_alocado)
{
    (void)self;
    LlamaCppConfig *cfg = obter_config();
    char *erro_cfg = llama_cpp_config_validar(cfg);
    if (erro_cfg) {
        if (detalhe_alocado) *detalhe_alocado = g_strdup_printf("configuração inválida: %s", erro_cfg);
        g_free(erro_cfg);
        return FALSE;
    }
    GSocketClient *cliente = g_socket_client_new();
    g_socket_client_set_timeout(cliente, 2);
    GError *erro = NULL;
    GSocketConnection *conexao = g_socket_client_connect_to_host(cliente, cfg->host, (guint16)cfg->porta, NULL, &erro);
    gboolean ok = conexao != NULL;
    if (detalhe_alocado) {
        *detalhe_alocado = g_strdup_printf("llama-server em %s:%d: %s",
            cfg->host, cfg->porta, ok ? "alcançável" : (erro ? erro->message : "indisponível"));
    }
    if (conexao) { g_io_stream_close(G_IO_STREAM(conexao), NULL, NULL); g_object_unref(conexao); }
    if (erro) g_error_free(erro);
    g_object_unref(cliente);
    return ok;
}

static ModeloBackend MODELO_LLAMA_CPP = {
    .nome_backend = "llama_cpp",
    .perguntar = llama_cpp_perguntar,
    .cancelar = llama_cpp_cancelar_pergunta,
    .disponivel = llama_cpp_checar_disponivel,
};

const ModeloBackend *modelo_llama_cpp_obter(void) { return &MODELO_LLAMA_CPP; }
