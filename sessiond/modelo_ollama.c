/* lia-sessiond — backend de modelo real sobre ollama/cliente_ollama.c.
 *
 * Configuração só por variável de ambiente (nunca por conteúdo de
 * arquivo/prompt — isso seria o próprio modelo controlando onde ele se
 * conecta): LIA_OLLAMA_HOST (padrão "127.0.0.1", só aceita
 * "127.0.0.1"/"localhost" — docs/THREAT-MODEL.md), LIA_OLLAMA_PORTA
 * (padrão 11434), LIA_OLLAMA_MODELO (padrão "llama3"),
 * LIA_OLLAMA_TIMEOUT_MS (padrão 60000 — CPU-only costuma precisar bem
 * mais que o antigo padrão de 15000, que estourava rotineiramente antes
 * do primeiro token chegar).
 *
 * Extrai, do texto de resposta, no máximo um bloco JSON de "proposta de
 * ferramenta" ({"ferramenta":...,"parametros":{...},"justificativa":...})
 * se o modelo tiver incluído um — nunca interpreta o restante do texto
 * como instrução, nunca chama nada sozinho com isso (sessiond/modelo.h).
 */
#include "modelo.h"
#include "../ollama/cliente_ollama.h"
#include <gio/gio.h>
#include <stdlib.h>

typedef struct {
    char *pergunta_id;
    ModeloConcluidoCb concluido_cb;
    gpointer ctx;
    OllamaRequisicao *req;
} TarefaOllama;

static OllamaConfig *g_cfg;
static GHashTable *g_em_andamento; /* pergunta_id -> TarefaOllama* */

static OllamaConfig *obter_config(void)
{
    if (g_cfg)
        return g_cfg;
    const char *host = g_getenv("LIA_OLLAMA_HOST");
    const char *modelo = g_getenv("LIA_OLLAMA_MODELO");
    const char *porta_s = g_getenv("LIA_OLLAMA_PORTA");
    const char *timeout_s = g_getenv("LIA_OLLAMA_TIMEOUT_MS");
    g_cfg = ollama_config_novo(host, porta_s ? atoi(porta_s) : 11434, modelo);
    if (timeout_s) g_cfg->timeout_ms = atoi(timeout_s);
    return g_cfg;
}

static void ao_concluir_ollama(gpointer ctx, OllamaErro erro, const char *texto_completo)
{
    TarefaOllama *t = ctx;
    ModeloResposta r = { 0 };

    if (erro == OLLAMA_OK) {
        r.sucesso = TRUE;
        modelo_preencher_proposta_de_texto(&r, texto_completo);
    } else {
        r.sucesso = FALSE;
        r.erro_codigo = g_strdup(
            erro == OLLAMA_ERRO_CANCELADO ? "cancelado" :
            erro == OLLAMA_ERRO_TIMEOUT ? "limite_excedido" :
            erro == OLLAMA_ERRO_LIMITE_EXCEDIDO ? "limite_excedido" :
            erro == OLLAMA_ERRO_CONEXAO ? "modelo_indisponivel" :
            "modelo_resposta_invalida");
        r.erro_mensagem = g_strdup(ollama_erro_texto(erro));
    }

    t->concluido_cb(t->ctx, &r);
    modelo_resposta_liberar(&r);

    if (g_em_andamento)
        g_hash_table_remove(g_em_andamento, t->pergunta_id);
    g_free(t->pergunta_id);
    g_free(t);
}

static void ollama_perguntar(const ModeloBackend *self, const char *pergunta_id,
                              const char *prompt_montado, ModeloConcluidoCb concluido_cb, gpointer ctx)
{
    (void)self;
    if (!g_em_andamento)
        g_em_andamento = g_hash_table_new(g_str_hash, g_str_equal);

    TarefaOllama *t = g_new0(TarefaOllama, 1);
    t->pergunta_id = g_strdup(pergunta_id);
    t->concluido_cb = concluido_cb;
    t->ctx = ctx;
    g_hash_table_insert(g_em_andamento, t->pergunta_id, t);
    t->req = ollama_gerar(obter_config(), prompt_montado, ao_concluir_ollama, t);
}

static void ollama_cancelar_pergunta(const ModeloBackend *self, const char *pergunta_id)
{
    (void)self;
    if (!g_em_andamento)
        return;
    TarefaOllama *t = g_hash_table_lookup(g_em_andamento, pergunta_id);
    if (t && t->req)
        ollama_cancelar(t->req);
}

static gboolean ollama_checar_disponivel(const ModeloBackend *self, char **detalhe_alocado)
{
    (void)self;
    OllamaConfig *cfg = obter_config();
    char *erro_cfg = ollama_config_validar(cfg);
    if (erro_cfg) {
        /* configuração inválida (host não-loopback, porta/timeout fora da
         * faixa, modelo malformado) — nunca chega a abrir socket nenhum,
         * nem para o diagnóstico de status (docs/THREAT-MODEL.md §5.2). */
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
        *detalhe_alocado = g_strdup_printf("ollama em %s:%d, modelo '%s': %s",
            cfg->host, cfg->porta, cfg->modelo, ok ? "alcançável" : (erro ? erro->message : "indisponível"));
    }
    if (conexao) { g_io_stream_close(G_IO_STREAM(conexao), NULL, NULL); g_object_unref(conexao); }
    if (erro) g_error_free(erro);
    g_object_unref(cliente);
    return ok;
}

static ModeloBackend MODELO_OLLAMA = {
    .nome_backend = "ollama",
    .perguntar = ollama_perguntar,
    .cancelar = ollama_cancelar_pergunta,
    .disponivel = ollama_checar_disponivel,
};

const ModeloBackend *modelo_ollama_obter(void) { return &MODELO_OLLAMA; }
