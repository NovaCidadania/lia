/* lia-sessiond — backend EXPERIMENTAL "lia_proprio": o modelo neural treinado do zero pela Lia (modelo-proprio/).
 *
 * Opt-in explícito (LIA_SESSIOND_MODELO=lia_proprio); nunca é o padrão e nunca substitui os outros backends.
 * Configuração só por ambiente:
 *   LIA_PROPRIO_CHECKPOINT   (obrigatório) arquivo .pt produzido por `lia-neural treinar`
 *   LIA_PROPRIO_CLI          (padrão "modelo-proprio/lia-neural") o executável que carrega o checkpoint e infere offline
 *   LIA_PROPRIO_TIMEOUT_MS   (padrão 120000)
 *
 * Contrato (docs/MODELO-PROPRIO.md): o modelo só PROPÕE uma edição estruturada {"edicoes":[...]}; quem aplica, mostra o diff,
 * pede aprovação, escreve e roda os testes é o runtime da Lia, igual a qualquer outro backend. Como todos os backends, este
 * módulo não enxerga executor.h nem sessiond.c.
 *
 * Checkpoint ausente/inválido FALHA com erro claro ("modelo_indisponivel"); não há recaída em outro modelo.
 * Só entende prompts de edição de código (um arquivo curto); o resumo final em texto livre (opcoes->resumo sem esquema) é recusado, e o runtime usa o
 * texto determinístico — o modelo nunca declara uma tarefa concluída. */
#include "modelo.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/resource.h>

typedef struct {
    char *pergunta_id;
    ModeloConcluidoCb concluido_cb;
    gpointer ctx;
    GSubprocess *proc;
    GCancellable *cancel;
    guint timeout_id;
    gboolean expirou;
} TarefaProprio;

static GHashTable *g_em_andamento; /* pergunta_id -> TarefaProprio* */
static char *g_identidade;

static const char *checkpoint_env(void) { const char *c = g_getenv("LIA_PROPRIO_CHECKPOINT"); return c && *c ? c : NULL; }
static const char *cli_env(void) { const char *c = g_getenv("LIA_PROPRIO_CLI"); return c && *c ? c : "modelo-proprio/lia-neural"; }

/* "Lia própria · <arquivo> · sha256 <12 hex>" — mostrado em toda resposta que usa o backend; NULL se não houver checkpoint legível */
char *modelo_lia_proprio_identidade(void)
{
    const char *ck = checkpoint_env();
    if (!ck) return NULL;
    if (!g_identidade) {
        char *dados = NULL; gsize n = 0;
        if (!g_file_get_contents(ck, &dados, &n, NULL)) return NULL;
        char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)dados, n);
        char *base = g_path_get_basename(ck);
        g_identidade = g_strdup_printf("Lia própria (experimental) · %s · sha256 %.12s", base, sha);
        g_free(dados); g_free(sha); g_free(base);
    }
    return g_strdup(g_identidade);
}

/* o daemon roda com RLIMIT_AS "mole" de 512 MB e "duro" maior (sessiond.c); o PyTorch precisa de mais espaço virtual que isso.
 * No filho (antes do exec) o limite mole sobe até o duro que o próprio daemon já tolera para processos filhos — nunca além dele. */
static void filho_sobe_limite_mole(gpointer dados)
{
    (void)dados;
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_AS, &rl);
    }
}

static void liberar(TarefaProprio *t)
{
    if (t->timeout_id) g_source_remove(t->timeout_id);
    if (g_em_andamento) g_hash_table_remove(g_em_andamento, t->pergunta_id);
    g_clear_object(&t->proc);
    g_clear_object(&t->cancel);
    g_free(t->pergunta_id);
    g_free(t);
}

static void concluir_erro(TarefaProprio *t, const char *codigo, const char *fim, const char *msg)
{
    ModeloResposta r = { 0 };
    r.sucesso = FALSE;
    r.erro_codigo = g_strdup(codigo);
    r.erro_mensagem = g_strdup(msg);
    r.fim = g_strdup(fim);
    r.tokens_gerados = -1;
    t->concluido_cb(t->ctx, &r);
    modelo_resposta_liberar(&r);
}

/* a saída do CLI é {"edicoes":[...],"explicacao":"...","modelo":{...},"motivo":"..."}; o texto entregue ao runtime é só o
 * contrato de edições (o que o runtime já sabe validar); sem edição, "edicoes" vem vazio e a explicação diz por quê */
static char *contrato_de_edicoes(const char *saida)
{
    JsonParser *p = json_parser_new();
    char *texto = NULL;
    if (json_parser_load_from_data(p, saida, -1, NULL)) {
        JsonNode *raiz = json_parser_get_root(p);
        if (raiz && JSON_NODE_TYPE(raiz) == JSON_NODE_OBJECT) {
            JsonObject *o = json_node_get_object(raiz);
            if (json_object_has_member(o, "edicoes") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, "edicoes"))) {
                JsonBuilder *b = json_builder_new();
                json_builder_begin_object(b);
                json_builder_set_member_name(b, "edicoes");
                json_builder_add_value(b, json_node_copy(json_object_get_member(o, "edicoes")));
                json_builder_set_member_name(b, "explicacao");
                const char *ex = json_object_has_member(o, "explicacao") ? json_object_get_string_member(o, "explicacao") : "";
                json_builder_add_string_value(b, ex ? ex : "");
                json_builder_end_object(b);
                JsonGenerator *g = json_generator_new();
                JsonNode *n = json_builder_get_root(b);
                json_generator_set_root(g, n);
                texto = json_generator_to_data(g, NULL);
                json_node_unref(n); g_object_unref(g); g_object_unref(b);
            }
        }
    }
    g_object_unref(p);
    return texto;
}

static void ao_terminar(GObject *src, GAsyncResult *res, gpointer ud)
{
    TarefaProprio *t = ud;
    char *out = NULL, *err = NULL;
    GError *e = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, &err, &e);
    if (t->expirou) {
        concluir_erro(t, "limite_excedido", "timeout", "o modelo próprio excedeu o tempo limite");
    } else if (!ok) {
        gboolean canc = e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        concluir_erro(t, canc ? "cancelado" : "modelo_indisponivel", canc ? "cancelado" : "transporte", e ? e->message : "falha ao falar com o CLI");
    } else if (!g_subprocess_get_if_exited(t->proc) || g_subprocess_get_exit_status(t->proc) != 0) {
        int rc = g_subprocess_get_if_exited(t->proc) ? g_subprocess_get_exit_status(t->proc) : -1;
        char *m = g_strdup_printf("lia-neural saiu com %d: %.300s", rc, err ? err : "");
        concluir_erro(t, rc == 3 ? "modelo_indisponivel" : "modelo_resposta_invalida", rc == 3 ? "transporte" : "resposta_invalida", m);
        g_free(m);
    } else {
        char *contrato = contrato_de_edicoes(out ? out : "");
        if (!contrato) {
            concluir_erro(t, "modelo_resposta_invalida", "resposta_invalida", "saída do CLI sem o contrato de edições");
        } else {
            ModeloResposta r = { 0 };
            r.sucesso = TRUE;
            r.resposta_texto = contrato;           /* só o JSON de edições; nenhuma "conclusão" */
            r.fim = g_strdup("completo");
            r.tokens_gerados = -1;
            t->concluido_cb(t->ctx, &r);
            modelo_resposta_liberar(&r);
        }
    }
    g_free(out); g_free(err);
    if (e) g_error_free(e);
    liberar(t);
}

static gboolean ao_expirar(gpointer ud)
{
    TarefaProprio *t = ud;
    t->timeout_id = 0;
    t->expirou = TRUE;
    if (t->proc) g_subprocess_force_exit(t->proc);
    return G_SOURCE_REMOVE;
}

static void proprio_perguntar(const ModeloBackend *self, const char *pergunta_id, const char *prompt_montado,
                              const ModeloOpcoes *opcoes, ModeloConcluidoCb concluido_cb, gpointer ctx)
{
    (void)self;
    TarefaProprio *t = g_new0(TarefaProprio, 1);
    t->pergunta_id = g_strdup(pergunta_id);
    t->concluido_cb = concluido_cb;
    t->ctx = ctx;
    if (!g_em_andamento) g_em_andamento = g_hash_table_new(g_str_hash, g_str_equal);

    /* O pedido de edições do coding também vem com .resumo=TRUE (texto cru, sem extração de proposta), mas com esquema_json; o
     * resumo FINAL em texto livre não tem esquema. O modelo só propõe edições: resumo/"concluído" é do runtime (texto determinístico). */
    if (opcoes && opcoes->resumo && !opcoes->esquema_json) {
        concluir_erro(t, "fora_do_escopo", "resposta_invalida", "o modelo próprio só propõe edições de código; não resume nem conclui");
        liberar(t);
        return;
    }
    const char *ck = checkpoint_env();
    if (!ck || !g_file_test(ck, G_FILE_TEST_IS_REGULAR)) {
        char *m = g_strdup_printf("checkpoint do modelo próprio ausente: defina LIA_PROPRIO_CHECKPOINT com um .pt válido (%s)", ck ? ck : "variável não definida");
        concluir_erro(t, "modelo_indisponivel", "transporte", m);
        g_free(m); liberar(t);
        return;
    }
    GError *e = NULL;
    GSubprocessLauncher *lc = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_child_setup(lc, filho_sobe_limite_mole, NULL, NULL);
    t->proc = g_subprocess_launcher_spawn(lc, &e, cli_env(), "propor-lia", "--checkpoint", ck, NULL);
    g_object_unref(lc);
    if (!t->proc) {
        char *m = g_strdup_printf("não consegui iniciar %s: %s", cli_env(), e ? e->message : "?");
        concluir_erro(t, "modelo_indisponivel", "transporte", m);
        g_free(m); if (e) g_error_free(e); liberar(t);
        return;
    }
    const char *to = g_getenv("LIA_PROPRIO_TIMEOUT_MS");
    int ms = to ? atoi(to) : 120000;
    t->timeout_id = g_timeout_add((guint)(ms > 0 ? ms : 120000), ao_expirar, t);
    t->cancel = g_cancellable_new();
    g_hash_table_insert(g_em_andamento, t->pergunta_id, t);
    g_subprocess_communicate_utf8_async(t->proc, prompt_montado, t->cancel, ao_terminar, t);
}

static void proprio_cancelar(const ModeloBackend *self, const char *pergunta_id)
{
    (void)self;
    if (!g_em_andamento) return;
    TarefaProprio *t = g_hash_table_lookup(g_em_andamento, pergunta_id);
    if (t) {
        if (t->cancel) g_cancellable_cancel(t->cancel);
        if (t->proc) g_subprocess_force_exit(t->proc);
    }
}

static gboolean proprio_disponivel(const ModeloBackend *self, char **detalhe)
{
    (void)self;
    const char *ck = checkpoint_env();
    gboolean ok = ck && g_file_test(ck, G_FILE_TEST_IS_REGULAR) && g_file_test(cli_env(), G_FILE_TEST_IS_EXECUTABLE);
    if (detalhe) *detalhe = ok ? g_strdup_printf("modelo próprio (experimental): checkpoint %s, CLI %s", ck, cli_env())
                               : g_strdup_printf("modelo próprio indisponível: checkpoint=%s cli=%s", ck ? ck : "(LIA_PROPRIO_CHECKPOINT não definida)", cli_env());
    return ok;
}

static ModeloBackend MODELO_LIA_PROPRIO = {
    .nome_backend = "lia_proprio",
    .perguntar = proprio_perguntar,
    .cancelar = proprio_cancelar,
    .disponivel = proprio_disponivel,
};

const ModeloBackend *modelo_lia_proprio_obter(void) { return &MODELO_LIA_PROPRIO; }
