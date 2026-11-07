/* lia-sessiond — daemon de sessão local (servidor de protocolo + máquina
 * de estados), ver docs/protocolo.md.
 *
 * O executor é plugável (sessiond/executor.h): por padrão, o real
 * (sessiond/executor_real.c, chama cli/sistema.c e cli/ferramentas.c de
 * verdade, limitado às seis ferramentas de leitura de
 * sessiond/manifesto_real.c); LIA_SESSIOND_EXECUTOR=mock troca para o
 * simulado (sessiond/executor_mock.c, sessiond/manifesto_mock.c), usado só
 * nos testes unitários do protocolo que precisam de aprovação/risco admin
 * sem depender de ferramenta real nenhuma.
 *
 * Nunca chama shell, system, popen, fork, execve, posix_spawn, sudo ou
 * pkexec diretamente — a única excessão, escopada e documentada, é o
 * fork+execve que cli/ferramentas.c já usa para sondar o catálogo fixo de
 * probes seguros de 'tools.probe --safe' (docs/protocolo.md §8,
 * docs/THREAT-MODEL.md §5.1). Nunca acessa hardware, nunca pede
 * privilégio, nunca interpreta texto de 'dados' como instrução de sistema
 * — só como dado estruturado do protocolo ("dado, nunca instrução").
 *
 * Processo comum (libc + GLib/GIO/json-glib), fora do núcleo sem libc —
 * mesma decisão de arquitetura da GUI (docs/GUI-POC.md): camada de
 * protocolo/orquestração, não faz parte de nucleo/runtime.
 */
#define _GNU_SOURCE

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../gui/protocolo.h"
#include "executor.h"
#include "modelo.h"
#include "prompt.h"

#define LIA_SESSIOND_LINHA_MAX 65536
#define LIA_SESSIOND_CHUNK 4096
#define LIA_SESSIOND_RLIMIT_AS_MB 512

typedef enum {
    ESTADO_CONECTADO = 0,
    ESTADO_PLANO_RECEBIDO,
    ESTADO_AGUARDANDO_APROVACAO,
    ESTADO_EXECUTANDO,
} EstadoSessao;

typedef struct Daemon Daemon;

typedef struct {
    Daemon *daemon;
    GSocketConnection *conexao;
    GInputStream *entrada;
    GOutputStream *saida;
    GString *buffer;
    char chunk[LIA_SESSIOND_CHUNK];
    gboolean fechado;
    guint refs; /* ver cliente_ref/cliente_unref: liberação só quando chega a 0 */

    GHashTable *ids_vistos; /* ids de mensagens recebidas nesta conexão */
    EstadoSessao estado;
    char *plano_id_pendente;
    char *ferramenta_pendente;
    char *execucao_id_atual;
    JsonNode *parametros_pendentes;
    guint timeout_execucao_id;

    /* ---- chat com o modelo (sessiond/modelo.h) — ver docs/protocolo.md
     * §9. Independente da máquina de estados de execução acima: uma
     * 'pergunta' nunca aprova nem executa nada por si só. */
    char *objetivo_pendente;      /* de 'solicitar'.dados.objetivo, opcional */
    char *justificativa_pendente; /* de 'solicitar'.dados.justificativa, opcional */
    gboolean origem_modelo;       /* 'solicitar'.dados.origem_modelo — liga ao pergunta_id_origem */
    char *pergunta_id_origem;     /* pergunta_id da 'proposta' que originou este solicitar, se houver */
    char *pergunta_pendente_id;   /* pergunta_id de um 'perguntar' ainda sem 'proposta' de resposta */
} Cliente;

struct Daemon {
    GSocketListener *listener;
    GMainLoop *loop;
    GList *clientes;
    char *caminho_socket;
    char *caminho_dir_run;
    char *caminho_dir_auditoria;
    gboolean encerrando;
};

static void desconectar_cliente(Cliente *c, const char *motivo);
static void comecar_leitura(Cliente *c);
static void disparar_resumo_modelo(Cliente *c, const char *ferramenta,
                                    const char *pergunta_id_origem, const char *resultado_texto);
static Daemon *g_daemon_para_auditoria; /* único daemon neste processo */
static const Executor *g_executor;
static const ModeloBackend *g_modelo;

/* ---------------------------------------------------------------- utils */

static const char *campo_string(JsonObject *obj, const char *chave)
{
    if (!obj || !json_object_has_member(obj, chave))
        return NULL;
    JsonNode *n = json_object_get_member(obj, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(n);
}

static JsonNode *obter_parametros_copia(JsonObject *dados)
{
    if (dados && json_object_has_member(dados, "parametros")) {
        JsonNode *n = json_object_get_member(dados, "parametros");
        if (JSON_NODE_TYPE(n) == JSON_NODE_OBJECT)
            return json_node_copy(n);
    }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_end_object(b);
    JsonNode *vazio = json_builder_get_root(b);
    g_object_unref(b);
    return vazio;
}

/* ------------------------------------------------------- refcontagem --- */
/* Cliente sobrevive enquanto algo ainda precisa dele: a lista do daemon
 * (1 referência, da criação até desconectar_cliente), e cada execução em
 * andamento (o 'iniciar' do executor e o watchdog de timeout seguram uma
 * referência cada, liberada quando o respectivo callback/handler termina).
 * Isto generaliza a correção de um heap-use-after-free encontrado com
 * AddressSanitizer na etapa anterior (liberação síncrona demais) para
 * também cobrir conclusões que chegam de uma thread de trabalho bem depois
 * de a conexão já ter caído. */
static Cliente *cliente_ref(Cliente *c) { c->refs++; return c; }

static void cliente_unref(Cliente *c)
{
    if (--c->refs > 0)
        return;
    g_hash_table_destroy(c->ids_vistos);
    g_string_free(c->buffer, TRUE);
    g_free(c->plano_id_pendente);
    g_free(c->ferramenta_pendente);
    g_free(c->execucao_id_atual);
    if (c->parametros_pendentes)
        json_node_unref(c->parametros_pendentes);
    g_free(c->objetivo_pendente);
    g_free(c->justificativa_pendente);
    g_free(c->pergunta_id_origem);
    g_free(c->pergunta_pendente_id);
    g_object_unref(c->conexao);
    g_free(c);
}

static void escrever_linha(Cliente *c, char *linha /* consome */)
{
    if (c->fechado) {
        g_free(linha);
        return;
    }
    GError *erro = NULL;
    gboolean ok = g_output_stream_write_all(c->saida, linha, strlen(linha), NULL, NULL, &erro);
    g_free(linha);
    if (!ok) {
        if (erro) g_error_free(erro);
        desconectar_cliente(c, "falha ao escrever no socket");
    }
}

/* --------------------------------------------------------- envio: msgs --- */

static void embutir_json_bruto(JsonBuilder *b, const char *chave, const char *json_texto)
{
    json_builder_set_member_name(b, chave);
    JsonParser *p = json_texto ? json_parser_new() : NULL;
    if (p && json_parser_load_from_data(p, json_texto, -1, NULL)) {
        json_builder_add_value(b, json_node_copy(json_parser_get_root(p)));
    } else {
        /* json_texto ausente OU malformado: falha segura — nunca propaga
         * JSON quebrado para o protocolo, mesmo que isso venha do backend. */
        json_builder_begin_object(b);
        json_builder_end_object(b);
    }
    if (p) g_object_unref(p);
}

static void enviar_erro(Cliente *c, const char *em_resposta_a, const char *codigo, const char *mensagem)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "codigo");
    json_builder_add_string_value(b, codigo);
    json_builder_set_member_name(b, "mensagem");
    json_builder_add_string_value(b, mensagem ? mensagem : "");
    json_builder_set_member_name(b, "detalhe");
    json_builder_add_null_value(b);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_ERRO, id, em_resposta_a, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void enviar_plano(Cliente *c, const char *em_resposta_a, const ExecutorManifesto *mf)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "plano_id");
    json_builder_add_string_value(b, c->plano_id_pendente);
    json_builder_set_member_name(b, "versao_manifesto");
    json_builder_add_string_value(b, mf->versao ? mf->versao : "0.0.0");
    json_builder_set_member_name(b, "objetivo");
    json_builder_add_string_value(b, c->objetivo_pendente ? c->objetivo_pendente : "");
    json_builder_set_member_name(b, "justificativa");
    if (c->justificativa_pendente)
        json_builder_add_string_value(b, c->justificativa_pendente);
    else
        json_builder_add_null_value(b);
    json_builder_set_member_name(b, "ferramenta");
    json_builder_add_string_value(b, mf->nome);
    json_builder_set_member_name(b, "parametros_normalizados");
    json_builder_add_value(b, json_node_copy(c->parametros_pendentes));
    /* "passos": nesta etapa, um plano sempre tem exatamente um passo — a
     * mesma ferramenta/parâmetros acima. A lista existe para o formato já
     * suportar múltiplos passos no futuro, sem quebrar clientes que leem
     * isto agora (docs/manifesto.md, estrutura do plano). */
    json_builder_set_member_name(b, "passos");
    json_builder_begin_array(b);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "ferramenta");
    json_builder_add_string_value(b, mf->nome);
    json_builder_set_member_name(b, "parametros");
    json_builder_add_value(b, json_node_copy(c->parametros_pendentes));
    json_builder_end_object(b);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "capacidades");
    json_builder_begin_array(b);
    for (int i = 0; mf->capacidades[i]; i++)
        json_builder_add_string_value(b, mf->capacidades[i]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "risco");
    json_builder_add_string_value(b, mf->risco);
    json_builder_set_member_name(b, "efeitos");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, mf->efeitos);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "idempotente");
    json_builder_add_boolean_value(b, mf->idempotente);
    json_builder_set_member_name(b, "timeout_padrao_ms");
    json_builder_add_int_value(b, mf->timeout_padrao_ms);
    json_builder_set_member_name(b, "reversivel");
    json_builder_add_string_value(b, mf->reversivel);
    json_builder_set_member_name(b, "requer_aprovacao");
    json_builder_add_boolean_value(b, mf->requer_aprovacao);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_PLANO, id, em_resposta_a, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void enviar_progresso(Cliente *c, double fracao, const char *mensagem)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "execucao_id");
    json_builder_add_string_value(b, c->execucao_id_atual);
    json_builder_set_member_name(b, "fracao");
    json_builder_add_double_value(b, fracao);
    json_builder_set_member_name(b, "mensagem");
    json_builder_add_string_value(b, mensagem);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_PROGRESSO, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void enviar_saida(Cliente *c, const char *trecho)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "execucao_id");
    json_builder_add_string_value(b, c->execucao_id_atual);
    json_builder_set_member_name(b, "fluxo");
    json_builder_add_string_value(b, "stdout");
    json_builder_set_member_name(b, "trecho");
    json_builder_add_string_value(b, trecho);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_SAIDA, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void enviar_resultado(Cliente *c, const char *estado, int codigo_saida, const char *dados_json)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "execucao_id");
    json_builder_add_string_value(b, c->execucao_id_atual);
    json_builder_set_member_name(b, "estado");
    json_builder_add_string_value(b, estado);
    json_builder_set_member_name(b, "codigo_saida");
    json_builder_add_int_value(b, codigo_saida);
    embutir_json_bruto(b, "dados", dados_json);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_RESULTADO, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void enviar_auditoria(Cliente *c, const char *resultado_resumo)
{
    const ExecutorManifesto *mf = g_executor->buscar_manifesto(c->ferramenta_pendente);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "entrada_id");
    char *entrada_id = lia_novo_id("audit");
    json_builder_add_string_value(b, entrada_id);
    g_free(entrada_id);
    json_builder_set_member_name(b, "ts");
    char *ts = lia_ts_agora();
    json_builder_add_string_value(b, ts);
    g_free(ts);
    json_builder_set_member_name(b, "identidade");
    char *uid_str = g_strdup_printf("uid=%u", (unsigned)geteuid());
    json_builder_add_string_value(b, uid_str);
    g_free(uid_str);
    json_builder_set_member_name(b, "ferramenta");
    json_builder_add_string_value(b, c->ferramenta_pendente ? c->ferramenta_pendente : "");
    json_builder_set_member_name(b, "parametros_normalizados");
    json_builder_add_value(b, json_node_copy(c->parametros_pendentes));
    json_builder_set_member_name(b, "capacidades");
    json_builder_begin_array(b);
    if (mf)
        for (int i = 0; mf->capacidades[i]; i++)
            json_builder_add_string_value(b, mf->capacidades[i]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "resultado_resumo");
    json_builder_add_string_value(b, resultado_resumo);
    json_builder_set_member_name(b, "reversao_disponivel");
    json_builder_add_boolean_value(b, mf && strcmp(mf->reversivel, "sim") == 0);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_AUDITORIA, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

/* Auditoria em disco: ~/.lia/humano/auditoria/AAAA-MM-DD.log, 0600,
 * append-only, SEM dados sensíveis (nunca parâmetros/conteúdo — só
 * identidade, ferramenta, risco e o resumo do resultado). Mesma convenção
 * de diretório que cli/privilegio.c já usa (THREAT-MODEL.md §7). */
static void registrar_auditoria_disco(Daemon *d, Cliente *c, const char *resultado_resumo)
{
    const ExecutorManifesto *mf = g_executor->buscar_manifesto(c->ferramenta_pendente);
    if (g_mkdir_with_parents(d->caminho_dir_auditoria, 0700) != 0 && errno != EEXIST)
        return;

    GDateTime *agora = g_date_time_new_now_utc();
    char *data = g_date_time_format(agora, "%Y-%m-%d");
    char *caminho = g_strdup_printf("%s/%s.log", d->caminho_dir_auditoria, data);
    char *ts_linha = g_date_time_format(agora, "%Y-%m-%d %H:%M:%S UTC");
    char *linha = g_strdup_printf(
        "%s uid=%u acao=sessiond.%s risco=%s resultado=%s\n",
        ts_linha,
        (unsigned)geteuid(),
        c->ferramenta_pendente ? c->ferramenta_pendente : "?",
        mf ? mf->risco : "?",
        resultado_resumo);

    int fd = open(caminho, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ssize_t n = write(fd, linha, strlen(linha));
        (void)n;
        close(fd);
    }
    g_free(linha);
    g_free(ts_linha);
    g_free(caminho);
    g_free(data);
    g_date_time_unref(agora);
}

/* --------------------------------------------------------- estado/fluxo */

static void resetar_para_conectado(Cliente *c)
{
    c->estado = ESTADO_CONECTADO;
    g_free(c->plano_id_pendente); c->plano_id_pendente = NULL;
    g_free(c->ferramenta_pendente); c->ferramenta_pendente = NULL;
    g_free(c->execucao_id_atual); c->execucao_id_atual = NULL;
    if (c->parametros_pendentes) { json_node_unref(c->parametros_pendentes); c->parametros_pendentes = NULL; }
    if (c->timeout_execucao_id) { g_source_remove(c->timeout_execucao_id); c->timeout_execucao_id = 0; }
    g_free(c->objetivo_pendente); c->objetivo_pendente = NULL;
    g_free(c->justificativa_pendente); c->justificativa_pendente = NULL;
    g_free(c->pergunta_id_origem); c->pergunta_id_origem = NULL;
    c->origem_modelo = FALSE;
}

/* Cancelamento de um PLANO (ainda não há execução — docs/protocolo.md: só
 * existe 'aprovar'/'rejeitar' nesse ponto). Gera um execucao_id só para o
 * 'resultado' ter o que referenciar, como o protocolo exige. */
static void finalizar_plano_cancelado(Cliente *c, const char *motivo)
{
    g_free(c->execucao_id_atual);
    c->execucao_id_atual = lia_novo_id("exec");
    enviar_resultado(c, "cancelado", 130, NULL);
    if (!c->fechado) enviar_auditoria(c, motivo);
    if (!c->fechado) registrar_auditoria_disco(g_daemon_para_auditoria, c, "cancelado");
    resetar_para_conectado(c);
}

/* ---- ctx compartilhado entre sessiond.c e os callbacks do Executor ---- */

typedef struct {
    Cliente *c;
    char *execucao_id;
} ExecCtx;

static ExecCtx *exec_ctx_novo(Cliente *c, const char *execucao_id)
{
    ExecCtx *e = g_new(ExecCtx, 1);
    e->c = cliente_ref(c);
    e->execucao_id = g_strdup(execucao_id);
    return e;
}

static gboolean exec_ctx_ainda_valido(ExecCtx *e)
{
    return !e->c->fechado && e->c->execucao_id_atual && strcmp(e->execucao_id, e->c->execucao_id_atual) == 0;
}

static void exec_ctx_liberar(ExecCtx *e)
{
    g_free(e->execucao_id);
    cliente_unref(e->c);
    g_free(e);
}

static void ao_saida_executor(gpointer ctx, const char *trecho)
{
    ExecCtx *e = ctx;
    if (exec_ctx_ainda_valido(e))
        enviar_saida(e->c, trecho);
}

static void ao_progresso_executor(gpointer ctx, double fracao, const char *mensagem)
{
    ExecCtx *e = ctx;
    if (exec_ctx_ainda_valido(e))
        enviar_progresso(e->c, fracao, mensagem);
}

static void ao_concluido_executor(gpointer ctx, const ExecutorResultado *r)
{
    ExecCtx *e = ctx;
    Cliente *c = e->c;

    if (exec_ctx_ainda_valido(e)) {
        gboolean foi_cancelado = r->erro_codigo && strcmp(r->erro_codigo, "cancelado") == 0;
        gboolean pedir_resumo = FALSE;
        char *resumo_texto = NULL;

        if (r->sucesso) {
            const char *estado = r->codigo_saida == 0 ? "sucesso" : "falha";
            enviar_resultado(c, estado, r->codigo_saida, r->dados_json);
            if (!c->fechado) enviar_auditoria(c, estado);
            if (!c->fechado) registrar_auditoria_disco(g_daemon_para_auditoria, c, estado);
            if (c->origem_modelo) {
                pedir_resumo = TRUE;
                resumo_texto = g_strdup_printf("estado=%s codigo_saida=%d dados=%s",
                    estado, r->codigo_saida, r->dados_json ? r->dados_json : "{}");
            }
        } else if (foi_cancelado) {
            enviar_resultado(c, "cancelado", r->codigo_saida, NULL);
            if (!c->fechado) enviar_auditoria(c, "cancelado");
            if (!c->fechado) registrar_auditoria_disco(g_daemon_para_auditoria, c, "cancelado");
        } else {
            enviar_erro(c, NULL, r->erro_codigo ? r->erro_codigo : "erro_interno", r->erro_mensagem);
            if (!c->fechado) enviar_resultado(c, "falha", r->codigo_saida, NULL);
            if (!c->fechado) enviar_auditoria(c, "falha");
            if (!c->fechado) registrar_auditoria_disco(g_daemon_para_auditoria, c, "falha");
            if (c->origem_modelo) {
                pedir_resumo = TRUE;
                resumo_texto = g_strdup_printf("falha: %s (%s)",
                    r->erro_mensagem ? r->erro_mensagem : "?", r->erro_codigo ? r->erro_codigo : "?");
            }
        }

        char *ferramenta_para_resumo = pedir_resumo ? g_strdup(c->ferramenta_pendente) : NULL;
        char *pergunta_id_para_resumo = pedir_resumo ? g_strdup(c->pergunta_id_origem) : NULL;
        resetar_para_conectado(c);
        if (pedir_resumo && !c->fechado)
            disparar_resumo_modelo(c, ferramenta_para_resumo, pergunta_id_para_resumo, resumo_texto);
        g_free(ferramenta_para_resumo);
        g_free(pergunta_id_para_resumo);
        g_free(resumo_texto);
    }
    /* se não é mais válido (timeout já resolveu, ou cliente caiu e a
     * conexão já foi inteiramente limpa), só solta os recursos — sem
     * reenviar nada para um estado que já foi encerrado. */
    exec_ctx_liberar(e);
}

static gboolean ao_timeout_execucao(gpointer ctx)
{
    ExecCtx *e = ctx;
    Cliente *c = e->c;
    if (exec_ctx_ainda_valido(e)) {
        /* O veredito do watchdog ("limite_excedido") é mandado e o estado
         * é resetado ANTES de pedir o cancelamento ao backend, de
         * propósito: g_executor->cancelar pode resolver de forma SÍNCRONA
         * (o backend mock sempre resolve assim, chamando concluido_cb —
         * ou seja, ao_concluido_executor — na hora). Se isso acontecesse
         * primeiro, o cliente veria dois resultados finais contraditórios
         * para a mesma execução ("cancelado" e depois "limite_excedido"),
         * violando o contrato de concluido_cb ser "exactly once". Nesta
         * ordem, quando ao_concluido_executor for chamado (síncrono ou
         * não), exec_ctx_ainda_valido() já vê c->execucao_id_atual limpo
         * e não manda nada — o veredito do watchdog já é definitivo. */
        enviar_erro(c, NULL, "limite_excedido", "tempo máximo de execução excedido");
        if (!c->fechado) enviar_resultado(c, "falha", 124, NULL);
        if (!c->fechado) enviar_auditoria(c, "limite_excedido");
        if (!c->fechado) registrar_auditoria_disco(g_daemon_para_auditoria, c, "limite_excedido");
        c->timeout_execucao_id = 0; /* este próprio timer: evita resetar_para_conectado removê-lo de novo */
        resetar_para_conectado(c);
        if (!c->fechado)
            g_executor->cancelar(g_executor, e->execucao_id);
    }
    return G_SOURCE_REMOVE;
}

static void despachar_execucao(Cliente *c, const ExecutorManifesto *mf)
{
    g_free(c->execucao_id_atual);
    c->execucao_id_atual = lia_novo_id("exec");
    c->estado = ESTADO_EXECUTANDO;

    JsonObject *params = c->parametros_pendentes ? json_node_get_object(c->parametros_pendentes) : NULL;

    g_executor->iniciar(g_executor, c->execucao_id_atual, mf, params,
                         ao_saida_executor, ao_progresso_executor, ao_concluido_executor,
                         exec_ctx_novo(c, c->execucao_id_atual));

    int timeout_ms = mf->timeout_padrao_ms > 0 ? mf->timeout_padrao_ms : 10000;
    c->timeout_execucao_id = g_timeout_add_full(G_PRIORITY_DEFAULT, (guint)timeout_ms,
                                                 ao_timeout_execucao, exec_ctx_novo(c, c->execucao_id_atual),
                                                 (GDestroyNotify)exec_ctx_liberar);
}

/* ----------------------------------------------------- chat com o modelo */
/* Ver sessiond/modelo.h e docs/THREAT-MODEL.md §5.2: o modelo nunca recebe
 * o socket, nunca aprova, nunca altera um plano — só propõe texto. Tudo
 * que vira efeito de verdade passa, sem exceção, pelo mesmo
 * tratar_solicitar/tratar_aprovar que qualquer 'solicitar' digitado por
 * uma pessoa usa. */

typedef struct {
    Cliente *c;
    char *pergunta_id;
} PerguntaCtx;

static PerguntaCtx *pergunta_ctx_novo(Cliente *c, const char *pergunta_id)
{
    PerguntaCtx *pc = g_new(PerguntaCtx, 1);
    pc->c = cliente_ref(c);
    pc->pergunta_id = g_strdup(pergunta_id);
    return pc;
}

static void pergunta_ctx_liberar(PerguntaCtx *pc)
{
    cliente_unref(pc->c);
    g_free(pc->pergunta_id);
    g_free(pc);
}

/* Lista as ferramentas do backend ATIVO (real ou mock) para a seção
 * "ferramentas disponíveis" do prompt — nunca hardcoded, para nunca
 * divergir do manifesto que tratar_solicitar realmente valida contra. */
static char *construir_manifesto_texto(void)
{
    GString *out = g_string_new(NULL);
    const char * const *nomes = g_executor->listar_nomes(g_executor);
    for (int i = 0; nomes[i]; i++) {
        const ExecutorManifesto *mf = g_executor->buscar_manifesto(nomes[i]);
        if (!mf) continue;
        g_string_append_printf(out, "- %s (risco: %s): %s\n", mf->nome, mf->risco, mf->efeitos);
    }
    return g_string_free(out, FALSE);
}

static void enviar_proposta(Cliente *c, const char *em_resposta_a, const char *pergunta_id, const ModeloResposta *r)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "pergunta_id");
    json_builder_add_string_value(b, pergunta_id);
    json_builder_set_member_name(b, "resposta_texto");
    json_builder_add_string_value(b, r->resposta_texto ? r->resposta_texto : "");
    json_builder_set_member_name(b, "ferramenta_proposta");
    if (r->ferramenta_proposta) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "ferramenta");
        json_builder_add_string_value(b, r->ferramenta_proposta);
        embutir_json_bruto(b, "parametros", r->parametros_json);
        json_builder_set_member_name(b, "justificativa");
        if (r->justificativa) json_builder_add_string_value(b, r->justificativa);
        else json_builder_add_null_value(b);
        json_builder_end_object(b);
    } else {
        json_builder_add_null_value(b);
    }
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_PROPOSTA, id, em_resposta_a, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void enviar_resposta_resumo(Cliente *c, const char *pergunta_id, const char *texto)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "pergunta_id");
    json_builder_add_string_value(b, pergunta_id);
    json_builder_set_member_name(b, "resposta_texto");
    json_builder_add_string_value(b, texto ? texto : "");
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_RESPOSTA, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void ao_concluido_pergunta(gpointer ctx, const ModeloResposta *r)
{
    PerguntaCtx *pc = ctx;
    Cliente *c = pc->c;
    gboolean ainda_valido = !c->fechado && c->pergunta_pendente_id &&
                             strcmp(pc->pergunta_id, c->pergunta_pendente_id) == 0;
    if (ainda_valido) {
        if (r->sucesso)
            enviar_proposta(c, NULL, pc->pergunta_id, r);
        else
            enviar_erro(c, NULL, r->erro_codigo ? r->erro_codigo : "modelo_resposta_invalida", r->erro_mensagem);
        g_free(c->pergunta_pendente_id);
        c->pergunta_pendente_id = NULL;
    }
    pergunta_ctx_liberar(pc);
}

static void ao_concluido_resumo(gpointer ctx, const ModeloResposta *r)
{
    PerguntaCtx *pc = ctx;
    Cliente *c = pc->c;
    if (!c->fechado) {
        if (r->sucesso)
            enviar_resposta_resumo(c, pc->pergunta_id, r->resposta_texto);
        else
            enviar_erro(c, NULL, r->erro_codigo ? r->erro_codigo : "modelo_resposta_invalida", r->erro_mensagem);
    }
    pergunta_ctx_liberar(pc);
}

/* Disparado depois que um 'resultado' de uma execução com origem_modelo
 * chega — pede ao modelo um resumo em texto; NUNCA propõe/aprova/executa
 * nada (mock_perguntar/ollama nem têm como: a função delas é só devolver
 * texto). Falha de forma segura (sem resumo, sem quebrar o resto) se o
 * modelo não estiver disponível ou o prompt não puder ser montado. */
static void disparar_resumo_modelo(Cliente *c, const char *ferramenta,
                                    const char *pergunta_id_origem, const char *resultado_texto)
{
    if (!g_modelo || !pergunta_id_origem)
        return;
    char *manifesto_texto = construir_manifesto_texto();
    char *pedido = g_strdup_printf(
        "A ferramenta '%s' que você sugeriu foi aprovada e executada. Resultado: %s\n"
        "Resuma isso em poucas frases para a pessoa; não sugira outra ferramenta agora.",
        ferramenta ? ferramenta : "?", resultado_texto ? resultado_texto : "(sem detalhes)");
    char *prompt_final = NULL;
    PromptErro pe = prompt_montar(manifesto_texto, pedido, NULL, &prompt_final);
    g_free(manifesto_texto);
    g_free(pedido);
    if (pe != PROMPT_OK) {
        g_free(prompt_final);
        return;
    }

    char *pergunta_id_nova = lia_novo_id("perg");
    PerguntaCtx *pc = pergunta_ctx_novo(c, pergunta_id_origem); /* referencia a pergunta ORIGINAL, para o cliente correlacionar */
    g_modelo->perguntar(g_modelo, pergunta_id_nova, prompt_final, ao_concluido_resumo, pc);
    g_free(pergunta_id_nova);
    g_free(prompt_final);
}

static void tratar_perguntar(Cliente *c, const LiaMsg *m)
{
    if (c->pergunta_pendente_id) {
        enviar_erro(c, m->id, "estado_invalido", "já existe uma pergunta em andamento nesta conexão");
        return;
    }
    const char *mensagem = campo_string(m->dados, "mensagem");
    if (!mensagem || !*mensagem) {
        enviar_erro(c, m->id, "parametro_invalido", "'mensagem' ausente ou vazia");
        return;
    }

    GList *arquivos = NULL;
    if (m->dados && json_object_has_member(m->dados, "arquivos")) {
        JsonNode *n = json_object_get_member(m->dados, "arquivos");
        if (JSON_NODE_TYPE(n) != JSON_NODE_ARRAY) {
            enviar_erro(c, m->id, "parametro_invalido", "'arquivos' deve ser uma lista");
            return;
        }
        JsonArray *arr = json_node_get_array(n);
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonNode *item = json_array_get_element(arr, i);
            if (JSON_NODE_TYPE(item) != JSON_NODE_OBJECT) continue;
            JsonObject *obj = json_node_get_object(item);
            arquivos = g_list_append(arquivos,
                prompt_arquivo_novo(campo_string(obj, "caminho"), campo_string(obj, "conteudo")));
        }
    }

    char *manifesto_texto = construir_manifesto_texto();
    char *prompt_final = NULL;
    PromptErro pe = prompt_montar(manifesto_texto, mensagem, arquivos, &prompt_final);
    g_free(manifesto_texto);
    g_list_free_full(arquivos, (GDestroyNotify)prompt_arquivo_liberar);

    if (pe != PROMPT_OK) {
        enviar_erro(c, m->id, "parametro_invalido", prompt_erro_texto(pe));
        g_free(prompt_final);
        return;
    }

    char *pergunta_id = lia_novo_id("perg");
    c->pergunta_pendente_id = g_strdup(pergunta_id);
    PerguntaCtx *pc = pergunta_ctx_novo(c, pergunta_id);
    g_modelo->perguntar(g_modelo, pergunta_id, prompt_final, ao_concluido_pergunta, pc);
    g_free(pergunta_id);
    g_free(prompt_final);
}

static void tratar_solicitar(Cliente *c, const LiaMsg *m)
{
    if (c->estado != ESTADO_CONECTADO) {
        enviar_erro(c, m->id, "estado_invalido", "já existe uma solicitação em andamento nesta conexão");
        return;
    }
    const char *ferramenta = campo_string(m->dados, "ferramenta");
    if (!ferramenta) {
        enviar_erro(c, m->id, "parametro_invalido", "'ferramenta' ausente ou não é string");
        return;
    }
    const ExecutorManifesto *mf = g_executor->buscar_manifesto(ferramenta);
    if (!mf) {
        enviar_erro(c, m->id, "ferramenta_desconhecida", ferramenta);
        return;
    }

    /* capacidades aceitas em produção (executor real) nesta etapa: só
     * leitura (docs/manifesto.md §2) — qualquer ferramenta com 'usuario',
     * 'admin' ou 'hardware' é rejeitada aqui, antes de qualquer validação
     * de parâmetro, mesmo que o manifesto real algum dia viesse a
     * oferecer uma (defesa em profundidade: hoje nenhuma entrada de
     * manifesto_real.c tem outra coisa). Esta política é do backend
     * 'real' (Executor.restringe_a_leitura) — o mock precisa poder testar
     * mock.escrita/mock.admin (risco médio/admin), que é exatamente o que
     * o real nunca tem; por isso não se aplica a ele. */
    if (g_executor->restringe_a_leitura) {
        for (int i = 0; mf->capacidades[i]; i++) {
            const char *cap = mf->capacidades[i];
            if (strcmp(cap, "leitura_arquivo") != 0 && strcmp(cap, "processo") != 0) {
                enviar_erro(c, m->id, "capacidade_negada", cap);
                return;
            }
        }
    }

    JsonObject *params_brutos = NULL;
    if (m->dados && json_object_has_member(m->dados, "parametros")) {
        JsonNode *n = json_object_get_member(m->dados, "parametros");
        if (JSON_NODE_TYPE(n) == JSON_NODE_OBJECT)
            params_brutos = json_node_get_object(n);
    }
    char *erro_param = g_executor->validar_parametros(mf, params_brutos);
    if (erro_param) {
        enviar_erro(c, m->id, "parametro_invalido", erro_param);
        g_free(erro_param);
        return;
    }

    /* 'objetivo'/'justificativa'/'origem_modelo'/'pergunta_id' são opcionais
     * e só informativos — nunca mudam validação/capacidade/aprovação. Uma
     * pessoa pode preenchê-los copiando o que uma 'proposta' do modelo
     * sugeriu, ou deixá-los de lado e montar o 'solicitar' do zero; o
     * caminho de validação é idêntico nos dois casos (docs/THREAT-MODEL.md
     * §5.2 — "o modelo não pode alterar o plano depois da aprovação": ele
     * nem chega a ESCREVER aqui, só a pessoa, via esta mesma mensagem). */
    g_free(c->objetivo_pendente);
    const char *objetivo = campo_string(m->dados, "objetivo");
    c->objetivo_pendente = objetivo ? g_strdup(objetivo) : NULL;
    g_free(c->justificativa_pendente);
    const char *justificativa = campo_string(m->dados, "justificativa");
    c->justificativa_pendente = justificativa ? g_strdup(justificativa) : NULL;
    gboolean pedido_origem_modelo = m->dados && json_object_has_member(m->dados, "origem_modelo") &&
        JSON_NODE_TYPE(json_object_get_member(m->dados, "origem_modelo")) == JSON_NODE_VALUE &&
        json_object_get_boolean_member(m->dados, "origem_modelo");
    g_free(c->pergunta_id_origem);
    const char *pergunta_id = campo_string(m->dados, "pergunta_id");
    c->origem_modelo = pedido_origem_modelo && pergunta_id != NULL;
    c->pergunta_id_origem = c->origem_modelo ? g_strdup(pergunta_id) : NULL;

    g_free(c->plano_id_pendente);
    c->plano_id_pendente = lia_novo_id("plano");
    g_free(c->ferramenta_pendente);
    c->ferramenta_pendente = g_strdup(ferramenta);
    if (c->parametros_pendentes) json_node_unref(c->parametros_pendentes);
    c->parametros_pendentes = obter_parametros_copia(m->dados);

    c->estado = ESTADO_PLANO_RECEBIDO;
    enviar_plano(c, m->id, mf);
    if (c->fechado) return;

    if (mf->requer_aprovacao) {
        c->estado = ESTADO_AGUARDANDO_APROVACAO;
    } else {
        despachar_execucao(c, mf);
    }
}

static void tratar_aprovar(Cliente *c, const LiaMsg *m)
{
    if (c->estado != ESTADO_AGUARDANDO_APROVACAO) {
        /* cobre replay de aprovação (o estado já saiu de
         * aguardando_aprovacao na primeira vez que foi processada) e
         * aprovação fora de ordem. */
        enviar_erro(c, m->id, "estado_invalido", "não há plano aguardando aprovação nesta conexão");
        return;
    }
    const char *plano_id = campo_string(m->dados, "plano_id");
    if (!plano_id || !c->plano_id_pendente || strcmp(plano_id, c->plano_id_pendente) != 0) {
        /* cobre aprovação de um plano diferente do apresentado. */
        enviar_erro(c, m->id, "estado_invalido", "plano_id não corresponde ao plano pendente");
        return;
    }
    const char *decisao = campo_string(m->dados, "decisao");
    if (!decisao || (strcmp(decisao, "aprovar") != 0 && strcmp(decisao, "rejeitar") != 0)) {
        enviar_erro(c, m->id, "parametro_invalido", "'decisao' deve ser 'aprovar' ou 'rejeitar'");
        return;
    }

    if (strcmp(decisao, "aprovar") == 0) {
        const ExecutorManifesto *mf = g_executor->buscar_manifesto(c->ferramenta_pendente);
        despachar_execucao(c, mf);
    } else {
        finalizar_plano_cancelado(c, "rejeitado pela pessoa");
    }
}

static void tratar_cancelar(Cliente *c, const LiaMsg *m)
{
    const char *exec_id_pedido = campo_string(m->dados, "execucao_id");

    if (c->estado == ESTADO_AGUARDANDO_APROVACAO) {
        if (exec_id_pedido && *exec_id_pedido) {
            enviar_erro(c, m->id, "estado_invalido",
                        "execucao_id não corresponde a nenhuma execução em andamento (plano ainda não aprovado)");
            return;
        }
        finalizar_plano_cancelado(c, "cancelado pelo cliente (antes da aprovação)");
    } else if (c->estado == ESTADO_EXECUTANDO) {
        if (!exec_id_pedido || !c->execucao_id_atual || strcmp(exec_id_pedido, c->execucao_id_atual) != 0) {
            enviar_erro(c, m->id, "estado_invalido", "execucao_id não corresponde à execução em andamento");
            return;
        }
        /* cooperativo: só pede; o resultado (estado=cancelado) chega via
         * ao_concluido_executor quando o backend reagir. */
        g_executor->cancelar(g_executor, c->execucao_id_atual);
    } else {
        enviar_erro(c, m->id, "estado_invalido", "nada em andamento para cancelar");
    }
}

static void tratar_assinar(Cliente *c, const LiaMsg *m)
{
    if (!m->dados || !json_object_has_member(m->dados, "canais")) {
        enviar_erro(c, m->id, "parametro_invalido", "'canais' ausente");
        return;
    }
    JsonNode *n_canais = json_object_get_member(m->dados, "canais");
    if (JSON_NODE_TYPE(n_canais) != JSON_NODE_ARRAY) {
        enviar_erro(c, m->id, "parametro_invalido", "'canais' deve ser uma lista");
        return;
    }
    JsonArray *arr = json_node_get_array(n_canais);
    guint n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
        const char *canal = json_array_get_string_element(arr, i);
        if (!canal || (strcmp(canal, "auditoria") != 0 && strcmp(canal, "execucao") != 0)) {
            enviar_erro(c, m->id, "parametro_invalido", "canal desconhecido em 'canais'");
            return;
        }
    }
    /* Aceito: nesta etapa não filtramos eventos por canal (sempre emitimos
     * tudo para a própria conexão que gerou a ação); 'assinar' só prova que
     * o cliente pode pedir e que canais inválidos são recusados. */
}

static void processar_linha(Cliente *c, const char *linha)
{
    LiaMsg m;
    LiaMsgErro erro_parse = lia_msg_parsear(linha, &m);
    if (erro_parse != LIA_MSG_OK) {
        const char *codigo = (erro_parse == LIA_MSG_ERRO_PROTOCOLO_INCOMPATIVEL)
            ? "protocolo_incompativel" : "mensagem_invalida";
        enviar_erro(c, NULL, codigo, lia_msg_erro_texto(erro_parse));
        if (!c->fechado)
            desconectar_cliente(c, "mensagem malformada (estrutura do envelope)");
        return;
    }

    if (g_hash_table_contains(c->ids_vistos, m.id)) {
        enviar_erro(c, m.id, "id_duplicado", "id já usado nesta sessão");
        lia_msg_liberar(&m);
        return;
    }
    g_hash_table_add(c->ids_vistos, g_strdup(m.id));

    /* o daemon só aceita os quatro tipos que docs/protocolo.md define como
     * cliente -> daemon; qualquer outro tipo (mesmo válido na tabela geral,
     * como 'plano' ou 'resultado') é fora de ordem vindo de um cliente. */
    switch (m.tipo) {
    case LIA_MSG_SOLICITAR: tratar_solicitar(c, &m); break;
    case LIA_MSG_APROVAR:   tratar_aprovar(c, &m); break;
    case LIA_MSG_CANCELAR:  tratar_cancelar(c, &m); break;
    case LIA_MSG_ASSINAR:   tratar_assinar(c, &m); break;
    case LIA_MSG_PERGUNTAR: tratar_perguntar(c, &m); break;
    default:
        enviar_erro(c, m.id, "estado_invalido", "tipo de mensagem não é permitido vindo do cliente");
        break;
    }
    lia_msg_liberar(&m);
}

/* ------------------------------------------------------------ conexão --- */

static void desconectar_cliente(Cliente *c, const char *motivo)
{
    if (c->fechado)
        return;
    c->fechado = TRUE;
    g_print("lia-sessiond: cliente desconectado (%s)\n", motivo ? motivo : "?");
    g_io_stream_close(G_IO_STREAM(c->conexao), NULL, NULL);
    c->daemon->clientes = g_list_remove(c->daemon->clientes, c);
    cliente_unref(c); /* solta a referência que a lista do daemon mantinha */
}

static void ao_ler_chunk(GObject *origem, GAsyncResult *res, gpointer dados)
{
    Cliente *c = cliente_ref(dados); /* mantém vivo durante todo este processamento */
    GError *erro = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(origem), res, &erro);
    if (n < 0) {
        if (erro) g_error_free(erro);
        desconectar_cliente(c, "erro de leitura");
        cliente_unref(c);
        return;
    }
    if (n == 0) {
        desconectar_cliente(c, "eof (cliente encerrou)");
        cliente_unref(c);
        return;
    }
    g_string_append_len(c->buffer, c->chunk, n);

    for (;;) {
        char *nl = memchr(c->buffer->str, '\n', c->buffer->len);
        if (!nl)
            break;
        gsize tam = (gsize)(nl - c->buffer->str);
        if (tam > LIA_SESSIOND_LINHA_MAX) {
            enviar_erro(c, NULL, "limite_excedido", "linha acima do limite de tamanho");
            if (!c->fechado)
                desconectar_cliente(c, "linha acima do limite");
            cliente_unref(c);
            return;
        }
        char *linha = g_strndup(c->buffer->str, tam);
        g_string_erase(c->buffer, 0, (gssize)(tam + 1));
        processar_linha(c, linha);
        g_free(linha);
        if (c->fechado) {
            cliente_unref(c);
            return;
        }
    }

    if (c->buffer->len > LIA_SESSIOND_LINHA_MAX) {
        enviar_erro(c, NULL, "limite_excedido", "linha acima do limite de tamanho (sem quebra de linha)");
        if (!c->fechado)
            desconectar_cliente(c, "linha longa sem terminador");
        cliente_unref(c);
        return;
    }
    comecar_leitura(c);
    cliente_unref(c);
}

static void comecar_leitura(Cliente *c)
{
    g_input_stream_read_async(c->entrada, c->chunk, sizeof(c->chunk),
                               G_PRIORITY_DEFAULT, NULL, ao_ler_chunk, c);
}

static Cliente *cliente_novo(Daemon *d, GSocketConnection *conexao)
{
    Cliente *c = g_new0(Cliente, 1);
    c->daemon = d;
    c->conexao = conexao;
    c->entrada = g_io_stream_get_input_stream(G_IO_STREAM(conexao));
    c->saida = g_io_stream_get_output_stream(G_IO_STREAM(conexao));
    c->buffer = g_string_new("");
    c->ids_vistos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    c->estado = ESTADO_CONECTADO;
    c->refs = 1; /* dono: a lista de clientes do daemon */
    return c;
}

/* checa SO_PEERCRED: só o mesmo uid do daemon pode conversar. Isto prova
 * SOMENTE identidade de sistema operacional — não é, por si só, autorização
 * de nenhuma ferramenta (ver docs/protocolo.md §7 e docs/THREAT-MODEL.md). */
static gboolean peer_uid_permitido(GSocketConnection *conexao)
{
    GSocket *sock = g_socket_connection_get_socket(conexao);
    int fd = g_socket_get_fd(sock);
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return FALSE; /* SO_PEERCRED indisponível: recusa por padrão (fail-safe) */
    return cred.uid == geteuid();
}

static void ao_aceitar(GObject *origem, GAsyncResult *res, gpointer dados)
{
    Daemon *d = dados;
    GError *erro = NULL;
    GSocketConnection *conexao = g_socket_listener_accept_finish(G_SOCKET_LISTENER(origem), res, NULL, &erro);
    if (!conexao) {
        if (erro) g_error_free(erro);
        /* listener fechado (encerramento) ou erro: não reagenda */
        return;
    }

    if (!peer_uid_permitido(conexao)) {
        g_print("lia-sessiond: conexão recusada (uid do cliente não corresponde ao uid do daemon)\n");
        GOutputStream *saida_tmp = g_io_stream_get_output_stream(G_IO_STREAM(conexao));
        const char *msg = "{\"protocolo\":\"lia.v1\",\"tipo\":\"erro\",\"id\":\"rejeitado\","
                           "\"em_resposta_a\":null,\"ts\":\"1970-01-01T00:00:00Z\","
                           "\"dados\":{\"codigo\":\"nao_autorizado\","
                           "\"mensagem\":\"uid do cliente nao corresponde ao uid do daemon\",\"detalhe\":null}}\n";
        g_output_stream_write_all(saida_tmp, msg, strlen(msg), NULL, NULL, NULL);
        g_io_stream_close(G_IO_STREAM(conexao), NULL, NULL);
        g_object_unref(conexao);
    } else {
        Cliente *c = cliente_novo(d, conexao);
        d->clientes = g_list_prepend(d->clientes, c);
        g_print("lia-sessiond: cliente conectado (%d ativo(s))\n", g_list_length(d->clientes));
        comecar_leitura(c);
    }

    if (!d->encerrando)
        g_socket_listener_accept_async(d->listener, NULL, ao_aceitar, d);
}

/* ------------------------------------------------------- socket/startup */

/* Remove um socket "stale" (arquivo de socket sem processo escutando) só
 * depois de validar que ele é mesmo nosso: mesmo uid, é de fato um socket,
 * e está dentro do nosso próprio diretório ~/.lia/run (já criado 0700 por
 * nós). Se outro daemon estiver de fato escutando, recusa inicializar em
 * vez de roubar o socket. */
static gboolean preparar_socket(const char *caminho, char **motivo_erro)
{
    struct stat st;
    if (lstat(caminho, &st) != 0) {
        if (errno == ENOENT)
            return TRUE; /* nada para limpar */
        *motivo_erro = g_strdup_printf("lstat(%s): %s", caminho, g_strerror(errno));
        return FALSE;
    }
    if (!S_ISSOCK(st.st_mode)) {
        *motivo_erro = g_strdup_printf("%s existe e não é um socket — não será removido", caminho);
        return FALSE;
    }
    if (st.st_uid != geteuid()) {
        *motivo_erro = g_strdup_printf("%s pertence a outro uid — não será removido", caminho);
        return FALSE;
    }

    /* é um socket nosso: ainda pode estar em uso. Testa conectando. */
    GSocketAddress *endereco = g_unix_socket_address_new(caminho);
    GSocketClient *cliente = g_socket_client_new();
    g_socket_client_set_timeout(cliente, 1);
    GError *erro = NULL;
    GSocketConnection *teste = g_socket_client_connect(cliente, G_SOCKET_CONNECTABLE(endereco), NULL, &erro);
    g_object_unref(endereco);
    g_object_unref(cliente);
    if (teste) {
        g_io_stream_close(G_IO_STREAM(teste), NULL, NULL);
        g_object_unref(teste);
        *motivo_erro = g_strdup_printf("já há um lia-sessiond escutando em %s", caminho);
        return FALSE;
    }
    if (erro) g_error_free(erro);

    /* validado: nosso, é socket, ninguém escutando -> stale, remove */
    if (g_unlink(caminho) != 0) {
        *motivo_erro = g_strdup_printf("não foi possível remover socket stale %s: %s", caminho, g_strerror(errno));
        return FALSE;
    }
    g_print("lia-sessiond: removido socket stale em %s\n", caminho);
    return TRUE;
}

static gboolean ao_sinal(gpointer dados)
{
    Daemon *d = dados;
    g_print("lia-sessiond: sinal recebido, encerrando...\n");
    d->encerrando = TRUE;
    if (d->listener)
        g_socket_listener_close(d->listener);
    for (GList *it = d->clientes; it; it = it->next) {
        Cliente *c = it->data;
        g_io_stream_close(G_IO_STREAM(c->conexao), NULL, NULL);
    }
    g_main_loop_quit(d->loop);
    return G_SOURCE_REMOVE;
}

static void escolher_executor(void)
{
    const char *modo = g_getenv("LIA_SESSIOND_EXECUTOR");
    if (modo && strcmp(modo, "mock") == 0) {
        g_executor = executor_mock_obter();
        g_print("lia-sessiond: executor = mock (LIA_SESSIOND_EXECUTOR=mock) — NUNCA o padrão em produção\n");
    } else {
        g_executor = executor_real_obter();
        g_print("lia-sessiond: executor = real (seis ferramentas de leitura, docs/manifesto.md)\n");
    }
}

/* Diferente do executor: "ollama" exige um servidor Ollama de verdade
 * rodando; "mock" não exige nada. Por isso, ao contrário do executor
 * (onde o padrão é "real", sem dependência extra), o padrão aqui é
 * "mock" — o modelo real é opt-in explícito (LIA_SESSIOND_MODELO=ollama),
 * nunca uma dependência silenciosa de produção. */
static void escolher_modelo(void)
{
    const char *modo = g_getenv("LIA_SESSIOND_MODELO");
    if (modo && strcmp(modo, "ollama") == 0) {
        g_modelo = modelo_ollama_obter();
        g_print("lia-sessiond: modelo = ollama (LIA_SESSIOND_MODELO=ollama)\n");
    } else {
        g_modelo = modelo_mock_obter();
        g_print("lia-sessiond: modelo = mock (padrão — defina LIA_SESSIOND_MODELO=ollama para o modelo real)\n");
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* stdout é, em operação normal, redirecionado para um arquivo de log
     * (não um terminal) — sem isto, a libc usa buffer cheio e as linhas só
     * apareceriam no arquivo quando o processo encerrasse, o que atrapalha
     * tanto quem acompanha o log ao vivo quanto os testes de integração
     * que leem o log enquanto o daemon ainda está rodando. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (geteuid() == 0) {
        g_printerr("lia-sessiond: recusa executar como root (ver docs/THREAT-MODEL.md §6)\n");
        return 1;
    }

    /* cli/sistema.c e cli/ferramentas.c (executor real) leem HOME via
     * lia_getenv, que por sua vez lê lia_envp — sem isto, toda chamada ao
     * executor real falharia silenciosamente com "HOME não definido". */
    extern char **lia_envp;
    extern char **environ;
    lia_envp = environ;
    /* defesa em profundidade: novos arquivos/dirs nascem restritos ao
     * próprio usuário (sem acesso de grupo/outros); 0077, não 0177 — um
     * umask que também removesse o bit de execução do dono quebraria a
     * criação de diretórios intermediários (mkdir -p precisa conseguir
     * entrar em cada nível para criar o próximo). */
    umask(0077);

    /* limite de memória do processo inteiro (defesa em profundidade; não é
     * por chamada — ver limitação documentada em docs/protocolo.md §8).
     * AddressSanitizer reserva um espaço de endereçamento virtual enorme
     * para a shadow memory (não é RSS real, mas conta para RLIMIT_AS) —
     * as duas coisas juntas sempre falham por desenho; a variável abaixo
     * existe só para os testes com ASan (testes/integration/), nunca para
     * uso normal, e não desliga nenhuma outra proteção do daemon. */
    if (!g_getenv("LIA_SESSIOND_SEM_RLIMIT_PARA_TESTE")) {
        struct rlimit rl = { .rlim_cur = (rlim_t)LIA_SESSIOND_RLIMIT_AS_MB * 1024 * 1024,
                              .rlim_max = (rlim_t)LIA_SESSIOND_RLIMIT_AS_MB * 1024 * 1024 };
        if (setrlimit(RLIMIT_AS, &rl) != 0)
            g_printerr("lia-sessiond: aviso: não foi possível aplicar RLIMIT_AS: %s\n", g_strerror(errno));
    }

    escolher_executor();
    escolher_modelo();

    Daemon d = { 0 };
    const char *home = g_get_home_dir();
    d.caminho_dir_run = g_strdup_printf("%s/.lia/run", home);
    d.caminho_socket = g_strdup_printf("%s/session.sock", d.caminho_dir_run);
    d.caminho_dir_auditoria = g_strdup_printf("%s/.lia/humano/auditoria", home);

    if (g_mkdir_with_parents(d.caminho_dir_run, 0700) != 0 && errno != EEXIST) {
        g_printerr("lia-sessiond: não foi possível criar %s: %s\n", d.caminho_dir_run, g_strerror(errno));
        return 1;
    }
    chmod(d.caminho_dir_run, 0700); /* independente do umask herdado */

    char *motivo_erro = NULL;
    if (!preparar_socket(d.caminho_socket, &motivo_erro)) {
        g_printerr("lia-sessiond: %s\n", motivo_erro);
        g_free(motivo_erro);
        return 1;
    }

    d.listener = g_socket_listener_new();
    GSocketAddress *endereco = g_unix_socket_address_new(d.caminho_socket);
    GError *erro = NULL;
    gboolean ok = g_socket_listener_add_address(d.listener, endereco, G_SOCKET_TYPE_STREAM,
                                                 G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &erro);
    g_object_unref(endereco);
    if (!ok) {
        g_printerr("lia-sessiond: não foi possível escutar em %s: %s\n", d.caminho_socket, erro->message);
        g_error_free(erro);
        return 1;
    }
    if (chmod(d.caminho_socket, 0600) != 0)
        g_printerr("lia-sessiond: aviso: chmod 0600 em %s falhou: %s\n", d.caminho_socket, g_strerror(errno));

    d.loop = g_main_loop_new(NULL, FALSE);
    g_daemon_para_auditoria = &d;

    g_unix_signal_add(SIGTERM, ao_sinal, &d);
    g_unix_signal_add(SIGINT, ao_sinal, &d);

    g_print("lia-sessiond: escutando em %s (pid %d)\n", d.caminho_socket, (int)getpid());
    g_socket_listener_accept_async(d.listener, NULL, ao_aceitar, &d);

    g_main_loop_run(d.loop);

    g_unlink(d.caminho_socket);
    g_print("lia-sessiond: encerrado, socket removido\n");

    g_list_free(d.clientes); /* clientes já fechados em ao_sinal; não há leak de struct intencional aqui nesta saída de processo */
    g_object_unref(d.listener);
    g_main_loop_unref(d.loop);
    g_free(d.caminho_socket);
    g_free(d.caminho_dir_run);
    g_free(d.caminho_dir_auditoria);
    return 0;
}
