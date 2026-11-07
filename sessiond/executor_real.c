/* lia-sessiond — backend real do Executor (sessiond/executor.h): chama
 * cmd_system/cmd_tools de verdade (cli/sistema.c, cli/ferramentas.c),
 * reaproveitando as mesmas funções que a CLI usa — ver
 * sessiond/lia_syscall_hospedado.c para a ponte com o núcleo sem libc.
 *
 * Nunca usa system/popen/fork/execve/posix_spawn/sudo/pkexec diretamente:
 * a única chamada de processo que existe em todo este backend é a que já
 * existia em cli/ferramentas.c (lia_fork/lia_execve) para o catálogo fixo
 * de probes seguros de `tools.probe --safe` — exceção escopada e
 * documentada em docs/protocolo.md §8 e docs/THREAT-MODEL.md §5.1.
 *
 * cli/sistema.c e cli/ferramentas.c usam estado global (lia_out/lia_err e
 * buffers estáticos próprios) e não são thread-safe entre si. Por isso
 * todo trabalho real roda serializado numa única thread dedicada
 * (g_async_queue + thread), nunca dois ao mesmo tempo, mesmo com duas
 * conexões simultâneas pedindo coisas diferentes — ver `thread_trabalho`.
 * Isso mantém o laço principal do GLib (e portanto todas as conexões)
 * responsivo mesmo que uma chamada demore (ex.: `tools.probe --safe`
 * sondando dezenas de binários).
 */
#include "executor.h"
#include "manifesto_real.h"
#include "../runtime/lia.h"
#include "../cli/cli.h"

#include <string.h>

#define SAIDA_CAP (4 * 1024 * 1024)
#define ERRO_CAP (8 * 1024)

typedef struct {
    char *execucao_id;
    const ManifestoReal *m;
    JsonObject *parametros; /* referência prestada; copiada via json_node_ref no nó pai antes de guardar */
    JsonNode *parametros_raiz; /* dono de 'parametros' acima */
    ExecutorConcluidoCb concluido_cb;
    gpointer ctx;
    volatile gint cancelado; /* g_atomic_int */
} TarefaReal;

typedef struct {
    ExecutorConcluidoCb concluido_cb;
    gpointer ctx;
    ExecutorResultado resultado;
} EntregaReal;

static GAsyncQueue *g_fila;
static GThread *g_thread;
static GMutex g_mutex_em_andamento;
static GHashTable *g_em_andamento; /* execucao_id (não possui) -> TarefaReal* */

static char *buf_saida, *buf_erro;

static gboolean campo_bool(JsonObject *obj, const char *chave, gboolean padrao)
{
    if (!obj || !json_object_has_member(obj, chave))
        return padrao;
    JsonNode *n = json_object_get_member(obj, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_BOOLEAN)
        return padrao;
    return json_node_get_boolean(n);
}

static const char *campo_str(JsonObject *obj, const char *chave)
{
    if (!obj || !json_object_has_member(obj, chave))
        return NULL;
    JsonNode *n = json_object_get_member(obj, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(n);
}

/* Roda a ferramenta de verdade. Preenche *r. Só é chamada de dentro da
 * thread dedicada (g_thread_trabalho) — nunca da thread principal. */
static void executar_de_verdade(const ManifestoReal *m, JsonObject *parametros, ExecutorResultado *r)
{
    b_init(&lia_out, buf_saida, SAIDA_CAP, -1);
    b_init(&lia_err, buf_erro, ERRO_CAP, -1);

    int rc;
    if (strcmp(m->base.nome, "tools.discover") == 0) {
        char *argv[] = { (char *)"discover" };
        rc = cmd_tools(1, argv);
    } else if (strcmp(m->base.nome, "tools.list") == 0) {
        const char *estado = campo_str(parametros, "estado");
        char *argv[4];
        int n = 0;
        argv[n++] = (char *)"list";
        argv[n++] = (char *)"--json";
        if (estado) {
            argv[n++] = (char *)"--estado";
            argv[n++] = (char *)estado;
        }
        rc = cmd_tools(n, argv);
    } else if (strcmp(m->base.nome, "tools.probe") == 0) {
        if (!campo_bool(parametros, "safe", TRUE)) {
            r->sucesso = FALSE;
            r->erro_codigo = g_strdup("parametro_invalido");
            r->erro_mensagem = g_strdup("'safe' só pode ser true nesta etapa — tools.probe sem --safe não existe");
            return;
        }
        char *argv[] = { (char *)"probe", (char *)"--safe" };
        rc = cmd_tools(2, argv);
    } else if (strcmp(m->base.nome, "sistema.diagnosticar") == 0) {
        char *argv[] = { (char *)"diagnosticar", (char *)"--json" };
        rc = cmd_system(2, argv);
    } else if (strcmp(m->base.nome, "sistema.recursos") == 0) {
        char *argv[] = { (char *)"recursos", (char *)"--json" };
        rc = cmd_system(2, argv);
    } else if (strcmp(m->base.nome, "sistema.dispositivos") == 0) {
        char *argv[] = { (char *)"dispositivos", (char *)"--json" };
        rc = cmd_system(2, argv);
    } else {
        r->sucesso = FALSE;
        r->erro_codigo = g_strdup("ferramenta_desconhecida");
        r->erro_mensagem = g_strdup(m->base.nome);
        return;
    }

    if (lia_out.err) {
        r->sucesso = FALSE;
        r->erro_codigo = g_strdup("limite_excedido");
        r->erro_mensagem = g_strdup("saída da ferramenta excedeu o limite capturado");
        return;
    }

    r->sucesso = TRUE;
    r->codigo_saida = rc;

    /* 'dados' do protocolo é a saída de texto/JSON da ferramenta, exatamente
     * como docs/protocolo.md §6 descreve ("a mesma saída --json como
     * payload de resultado.dados"). cli/sistema.c já produz um objeto
     * JSON; cli/ferramentas.c "list --json" produz um array; "discover" e
     * "probe" produzem texto humano (ainda não têm --json nesses
     * subcomandos) — envolvemos texto humano numa string JSON para que
     * 'dados' seja sempre JSON válido. */
    const char *nome = m->base.nome;
    gboolean ja_e_json = strcmp(nome, "tools.list") == 0 || strncmp(nome, "sistema.", 8) == 0;
    if (ja_e_json) {
        char *saida = g_strndup(lia_out.p, lia_out.n);
        r->dados_json = g_strdup_printf("{\"saida\":%s}", saida);
        g_free(saida);
    } else {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "texto");
        char *texto = g_strndup(lia_out.p, lia_out.n);
        json_builder_add_string_value(b, texto);
        g_free(texto);
        json_builder_end_object(b);
        JsonNode *n = json_builder_get_root(b);
        JsonGenerator *gen = json_generator_new();
        json_generator_set_root(gen, n);
        r->dados_json = json_generator_to_data(gen, NULL);
        json_node_unref(n);
        g_object_unref(gen);
        g_object_unref(b);
    }
}

static gboolean entregar_idle(gpointer dados)
{
    EntregaReal *e = dados;
    e->concluido_cb(e->ctx, &e->resultado);
    executor_resultado_liberar(&e->resultado);
    g_free(e);
    return G_SOURCE_REMOVE;
}

static gpointer loop_trabalhador(gpointer dados)
{
    (void)dados;
    for (;;) {
        TarefaReal *t = g_async_queue_pop(g_fila);
        if (!t)
            break; /* sentinela de encerramento */

        ExecutorResultado r = { 0 };
        if (g_atomic_int_get(&t->cancelado)) {
            r.sucesso = FALSE;
            r.codigo_saida = 130;
            r.erro_codigo = g_strdup("cancelado");
            r.erro_mensagem = g_strdup("cancelado antes de iniciar");
        } else {
            executar_de_verdade(t->m, t->parametros, &r);
            if (g_atomic_int_get(&t->cancelado)) {
                /* pedido de cancelamento chegou durante a execução: o
                 * resultado computado (mesmo que tenha sido sucesso) não
                 * é reportado como sucesso — cooperativo, não preemptivo
                 * (docs/protocolo.md §3, 'cancelar' é "cooperativo"). */
                executor_resultado_liberar(&r);
                memset(&r, 0, sizeof r);
                r.sucesso = FALSE;
                r.codigo_saida = 130;
                r.erro_codigo = g_strdup("cancelado");
                r.erro_mensagem = g_strdup("cancelado durante a execução");
            }
        }

        g_mutex_lock(&g_mutex_em_andamento);
        g_hash_table_remove(g_em_andamento, t->execucao_id);
        g_mutex_unlock(&g_mutex_em_andamento);

        EntregaReal *e = g_new(EntregaReal, 1);
        e->concluido_cb = t->concluido_cb;
        e->ctx = t->ctx;
        e->resultado = r;
        g_idle_add(entregar_idle, e);

        g_free(t->execucao_id);
        if (t->parametros_raiz)
            json_node_unref(t->parametros_raiz);
        g_free(t);
    }
    return NULL;
}

static void garantir_thread(void)
{
    if (g_thread)
        return;
    buf_saida = g_malloc(SAIDA_CAP);
    buf_erro = g_malloc(ERRO_CAP);
    g_fila = g_async_queue_new();
    g_em_andamento = g_hash_table_new(g_str_hash, g_str_equal);
    g_thread = g_thread_new("lia-executor-real", loop_trabalhador, NULL);
}

static const ExecutorManifesto *real_buscar_manifesto(const char *ferramenta)
{
    const ManifestoReal *m = manifesto_real_buscar(ferramenta);
    return m ? &m->base : NULL;
}

static const char * const *real_listar_nomes(const Executor *self)
{
    (void)self;
    return manifesto_real_listar_nomes();
}

static char *real_validar_parametros(const ExecutorManifesto *m, JsonObject *parametros)
{
    const ManifestoReal *mr = manifesto_real_buscar(m->nome);
    if (!mr)
        return g_strdup("ferramenta desconhecida (inconsistência interna)");
    return manifesto_real_validar_parametros(mr, parametros);
}

static void real_iniciar(const Executor *self, const char *execucao_id, const ExecutorManifesto *m,
                          JsonObject *parametros, ExecutorSaidaCb saida_cb, ExecutorProgressoCb progresso_cb,
                          ExecutorConcluidoCb concluido_cb, gpointer ctx)
{
    (void)self;
    (void)saida_cb; (void)progresso_cb; /* estas seis ferramentas são rápidas o bastante e
                                          * síncronas: a saída inteira vai em resultado.dados,
                                          * sem streaming incremental nesta etapa. */
    garantir_thread();

    const ManifestoReal *mr = manifesto_real_buscar(m->nome);
    TarefaReal *t = g_new0(TarefaReal, 1);
    t->execucao_id = g_strdup(execucao_id);
    t->m = mr;
    t->concluido_cb = concluido_cb;
    t->ctx = ctx;
    if (parametros) {
        JsonNode *n = json_node_alloc();
        json_node_init_object(n, parametros);
        t->parametros_raiz = n;
        t->parametros = parametros;
    }

    g_mutex_lock(&g_mutex_em_andamento);
    g_hash_table_insert(g_em_andamento, t->execucao_id, t);
    g_mutex_unlock(&g_mutex_em_andamento);

    g_async_queue_push(g_fila, t);
}

static void real_cancelar(const Executor *self, const char *execucao_id)
{
    (void)self;
    g_mutex_lock(&g_mutex_em_andamento);
    if (g_em_andamento) {
        TarefaReal *t = g_hash_table_lookup(g_em_andamento, execucao_id);
        if (t)
            g_atomic_int_set(&t->cancelado, 1);
    }
    g_mutex_unlock(&g_mutex_em_andamento);
}

static Executor EXECUTOR_REAL = {
    .nome_backend = "real",
    .restringe_a_leitura = TRUE,
    .buscar_manifesto = real_buscar_manifesto,
    .listar_nomes = real_listar_nomes,
    .validar_parametros = real_validar_parametros,
    .iniciar = real_iniciar,
    .cancelar = real_cancelar,
};

const Executor *executor_real_obter(void) { return &EXECUTOR_REAL; }
