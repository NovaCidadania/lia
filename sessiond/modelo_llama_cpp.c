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
#include <string.h>

typedef struct {
    char *pergunta_id;
    ModeloConcluidoCb concluido_cb;
    gpointer ctx;
    gboolean resumo; /* pedido de resumo final: a resposta é só texto, nunca uma proposta */
    LlamaCppFim fim; /* como a geração terminou (preenchido pelo cliente antes da conclusão) */
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
    /* só para avaliação (variância entre repetições); padrão: temperatura 0 */
    const char *te = g_getenv("LIA_MODELO_TEMPERATURA"), *se = g_getenv("LIA_MODELO_SEMENTE");
    if (te) g_cfg->temperatura = g_ascii_strtod(te, NULL);
    if (se) g_cfg->semente = g_ascii_strtoll(se, NULL, 10);
    return g_cfg;
}

static void ao_concluir_llama_cpp(gpointer ctx, LlamaCppErro erro, const char *texto_completo)
{
    TarefaLlamaCpp *t = ctx;
    ModeloResposta r = { 0 };

    if (erro == LLAMACPP_OK) {
        r.sucesso = TRUE;
        if (t->resumo) {
            /* resumo final: texto e nada mais — mesmo que o modelo escreva
             * algo com cara de proposta, NUNCA é interpretado como uma
             * (evita encadear ações sem um novo pedido da pessoa). */
            r.resposta_texto = g_strdup(texto_completo ? texto_completo : "");
            g_strstrip(r.resposta_texto);
        } else {
            modelo_preencher_proposta_estruturada(&r, texto_completo);
        }
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

    r.tokens_gerados = t->fim.tokens_gerados;
    r.fim = g_strdup(erro == LLAMACPP_OK ? (!strcmp(t->fim.motivo, "length") ? "limite_tokens" : "completo") :
                     erro == LLAMACPP_ERRO_CANCELADO ? "cancelado" :
                     erro == LLAMACPP_ERRO_TIMEOUT ? "timeout" :
                     erro == LLAMACPP_ERRO_LIMITE_EXCEDIDO ? "limite_bytes" :
                     erro == LLAMACPP_ERRO_RESPOSTA_INVALIDA ? "resposta_invalida" : "transporte");
    t->concluido_cb(t->ctx, &r);
    modelo_resposta_liberar(&r);

    if (g_em_andamento)
        g_hash_table_remove(g_em_andamento, t->pergunta_id);
    g_free(t->pergunta_id);
    g_free(t);
}

/* Modelos "instruct" esperam o modelo de conversa deles; sem isso, o
 * /completion só CONTINUA o texto (o modelo repete as regras do prompt em
 * vez de responder). LIA_LLAMACPP_FORMATO: "cru" (padrão, texto como está),
 * "chatml" (Qwen, muitos outros) ou "llama3". */
char *llama_cpp_formatar_prompt(const char *formato, const char *prompt)
{
    if (formato && !g_ascii_strcasecmp(formato, "chatml"))
        return g_strdup_printf("<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", prompt);
    if (formato && !g_ascii_strcasecmp(formato, "llama3"))
        return g_strdup_printf("<|start_header_id|>user<|end_header_id|>\n\n%s<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n", prompt);
    return g_strdup(prompt);
}

static void llama_cpp_perguntar(const ModeloBackend *self, const char *pergunta_id,
                                 const char *prompt_montado, const ModeloOpcoes *opcoes,
                              ModeloConcluidoCb concluido_cb, gpointer ctx)
{
    (void)self;
    if (!g_em_andamento)
        g_em_andamento = g_hash_table_new(g_str_hash, g_str_equal);

    TarefaLlamaCpp *t = g_new0(TarefaLlamaCpp, 1);
    t->pergunta_id = g_strdup(pergunta_id);
    t->concluido_cb = concluido_cb;
    t->ctx = ctx;
    g_hash_table_insert(g_em_andamento, t->pergunta_id, t);
    t->resumo = opcoes && opcoes->resumo;
    t->fim.tokens_gerados = t->fim.tokens_prompt = -1;
    char *prompt = llama_cpp_formatar_prompt(g_getenv("LIA_LLAMACPP_FORMATO"), prompt_montado);
    t->req = llama_cpp_gerar_opcoes(obter_config(), prompt, opcoes ? opcoes->esquema_json : NULL,
                                  opcoes ? opcoes->max_tokens : 0, ao_concluir_llama_cpp, t);
    if (t->req) llama_cpp_definir_fim(t->req, &t->fim);
    g_free(prompt);
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
