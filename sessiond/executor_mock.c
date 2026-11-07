/* lia-sessiond — backend mock do Executor (sessiond/executor.h), sobre a
 * tabela fictícia de sessiond/manifesto_mock.c. Nunca é o padrão em
 * produção (ver escolher_executor em sessiond.c) — só para os testes
 * unitários do protocolo que precisam exercitar aprovação/risco
 * admin/mutação/timeout/limite de saída sem depender de nenhuma
 * ferramenta real (docs/protocolo.md §8).
 */
#include "executor.h"
#include "manifesto_mock.h"
#include <string.h>

typedef struct {
    char *execucao_id;
    const ManifestoMock *mf;
    ExecutorProgressoCb progresso_cb;
    ExecutorConcluidoCb concluido_cb;
    gpointer ctx;
    int tick;
    guint timer_id; /* 0 quando não há timer pendente (nunca agendado, ou disparando agora) */
} TarefaMock;

static GHashTable *g_tarefas; /* execucao_id -> TarefaMock* */

static void tarefa_mock_liberar(TarefaMock *t)
{
    g_free(t->execucao_id);
    g_free(t);
}

/* Única saída de uma tarefa: entrega o resultado, remove da tabela e do
 * timer pendente (se houver) e libera. Chamada tanto pelo fim normal
 * (ao_tick) quanto por um cancelamento explícito (mock_cancelar) — nos
 * dois casos, imediata, nunca espera o próximo tick. */
static void concluir_tarefa(TarefaMock *t, ExecutorResultado *r)
{
    if (t->timer_id) { g_source_remove(t->timer_id); t->timer_id = 0; }
    t->concluido_cb(t->ctx, r);
    executor_resultado_liberar(r);
    g_hash_table_remove(g_tarefas, t->execucao_id);
    tarefa_mock_liberar(t);
}

static gboolean ao_tick(gpointer dados)
{
    TarefaMock *t = dados;
    t->timer_id = 0; /* este disparo já consumiu o source; concluir_tarefa não tenta remover de novo */
    t->tick++;

    if (t->mf->forcar_limite_saida) {
        ExecutorResultado r = { .sucesso = FALSE, .codigo_saida = 0,
                                 .erro_codigo = g_strdup("limite_excedido"),
                                 .erro_mensagem = g_strdup("saída simulada excederia o limite configurado (mock.saida_grande)") };
        concluir_tarefa(t, &r);
        return G_SOURCE_REMOVE;
    }
    if (t->tick == 1) {
        t->progresso_cb(t->ctx, 0.5, "simulando execução (mock)");
        t->timer_id = g_timeout_add(250, ao_tick, t);
        return G_SOURCE_REMOVE;
    }
    ExecutorResultado r = { .sucesso = TRUE, .codigo_saida = 0,
                             .dados_json = g_strdup_printf("{\"mock\":true,\"ferramenta\":\"%s\"}", t->mf->nome) };
    concluir_tarefa(t, &r);
    return G_SOURCE_REMOVE;
}

static const char * const *mock_listar_nomes(const Executor *self)
{
    (void)self;
    static const char *nomes[] = {
        "mock.leitura", "mock.escrita", "mock.admin", "mock.timeout", "mock.saida_grande", NULL,
    };
    return nomes;
}

static const ExecutorManifesto *mock_buscar_manifesto(const char *ferramenta)
{
    const ManifestoMock *mf = manifesto_mock_buscar(ferramenta);
    if (!mf)
        return NULL;
    static ExecutorManifesto saida;
    saida.nome = mf->nome;
    saida.versao = mf->versao;
    saida.risco = mf->risco;
    saida.requer_aprovacao = mf->requer_aprovacao;
    saida.idempotente = mf->idempotente;
    saida.reversivel = mf->reversivel;
    saida.capacidades = mf->capacidades;
    saida.efeitos = mf->efeito_simulado;
    saida.timeout_padrao_ms = mf->timeout_padrao_ms;
    return &saida;
}

static char *mock_validar_parametros(const ExecutorManifesto *m, JsonObject *parametros)
{
    (void)m; (void)parametros;
    return NULL; /* mock não tem parâmetros declarados a validar */
}

static void mock_iniciar(const Executor *self, const char *execucao_id, const ExecutorManifesto *m,
                          JsonObject *parametros, ExecutorSaidaCb saida_cb, ExecutorProgressoCb progresso_cb,
                          ExecutorConcluidoCb concluido_cb, gpointer ctx)
{
    (void)self; (void)parametros;
    if (!g_tarefas)
        g_tarefas = g_hash_table_new(g_str_hash, g_str_equal);

    TarefaMock *t = g_new0(TarefaMock, 1);
    t->execucao_id = g_strdup(execucao_id);
    t->mf = manifesto_mock_buscar(m->nome);
    t->progresso_cb = progresso_cb;
    t->concluido_cb = concluido_cb;
    t->ctx = ctx;
    g_hash_table_insert(g_tarefas, t->execucao_id, t);

    char *trecho = g_strdup_printf("[mock] %s", t->mf->efeito_simulado);
    saida_cb(ctx, trecho);
    g_free(trecho);

    /* mock.timeout (nunca_conclui): nenhum timer é agendado de propósito
     * — só o watchdog de timeout do próprio daemon (sessiond.c) resolve
     * isto, exercitando o caminho real de código, não uma simulação dele. */
    if (!t->mf->nunca_conclui)
        t->timer_id = g_timeout_add(250, ao_tick, t);
}

static void mock_cancelar(const Executor *self, const char *execucao_id)
{
    (void)self;
    if (!g_tarefas)
        return;
    TarefaMock *t = g_hash_table_lookup(g_tarefas, execucao_id);
    if (!t)
        return;
    ExecutorResultado r = { .sucesso = FALSE, .codigo_saida = 130,
                             .erro_codigo = g_strdup("cancelado"),
                             .erro_mensagem = g_strdup("cancelado (mock)") };
    concluir_tarefa(t, &r);
}

static Executor EXECUTOR_MOCK = {
    .nome_backend = "mock",
    .restringe_a_leitura = FALSE,
    .buscar_manifesto = mock_buscar_manifesto,
    .listar_nomes = mock_listar_nomes,
    .validar_parametros = mock_validar_parametros,
    .iniciar = mock_iniciar,
    .cancelar = mock_cancelar,
};

const Executor *executor_mock_obter(void) { return &EXECUTOR_MOCK; }
