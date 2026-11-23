/* lia-sessiond — daemon de sessão local (servidor de protocolo + máquina
 * de estados), ver docs/protocolo.md.
 *
 * O executor é plugável (sessiond/executor.h): por padrão, o real
 * (sessiond/executor_real.c, chama cli/sistema.c e cli/ferramentas.c de
 * verdade, limitado às sete ferramentas de leitura de
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
#include <malloc.h>
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
#include "roteador.h"
#include "apresentacao.h"
#include "agente.h"
#include "sintese_web.h"

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
typedef struct Tarefa Tarefa;

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
    char *pergunta_modelo_id;     /* id da requisição ao modelo em andamento para essa pergunta (muda na nova tentativa) — só para cancelar */

    /* ---- fluxo agente (docs/protocolo.md §10): a última pergunta
     * respondida com uma proposta fica guardada para o resumo final citar
     * o pedido original; resumo_pendente_id é a requisição de resumo em
     * andamento (para cancelar / descartar uma resposta atrasada). */
    char *ultima_pergunta_id;
    char *ultima_pergunta_texto;
    char *resumo_pendente_id;

    /* ---- Agent Runtime v2 (sessiond/agente.h, docs/protocolo.md §11) ----
     * plano_proposto: plano de vários passos já VALIDADO, enviado numa
     * 'proposta' e guardado aqui — é ESTE objeto que um 'solicitar' com
     * plano_id+hash executa; o cliente (e o modelo) nunca reenviam o
     * conteúdo do plano, então não têm como alterá-lo.
     * tarefa: a execução em andamento (no máximo uma por conexão). */
    AgentePlano *plano_proposto;
    char *plano_proposto_pergunta_id;
    char *plano_proposto_pedido;
    Tarefa *tarefa;
    char *plano_hash_pendente;    /* hash do plano de um passo (fluxo antigo): 'aprovar' precisa trazê-lo */
    gint64 exec_inicio_us;        /* g_get_monotonic_time() de quando a execução atual foi despachada */
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
static void disparar_resumo_modelo(Cliente *c, const char *ferramenta, const char *pergunta_id_origem,
                                    const char *apresentacao, const char *dados_json, gboolean truncado);
static void enviar_resposta_final(Cliente *c, const char *pergunta_id, const char *texto, const char *origem,
                                   gboolean fundamentada, const char *aviso, gint64 tempo_modelo_ms);
/* Agent Runtime v2 — definidos mais abaixo, junto da máquina de execução */
static const AgenteManifesto AG_MANIFESTO;
static void enviar_proposta_tarefa(Cliente *c, const char *pergunta_id, const AgentePlano *plano, AgenteCompat compat,
                                    const char *texto, const char *origem, const char *veredito, const char *motivo,
                                    gint64 tempo_ms, int tentativas);
static void guardar_plano_proposto(Cliente *c, AgentePlano *plano, const char *pergunta_id, const char *pedido);
static void aquecer_modelo(void);
static Daemon *g_daemon_para_auditoria; /* único daemon neste processo */
static const Executor *g_executor;
static const ModeloBackend *g_modelo;
/* LIA_SESSIOND_ROTEAMENTO: "direto" (padrão) — intenção de ALTA confiança
 * vira proposta na hora, pela rota determinística, sem consultar o modelo
 * para escolher a ferramenta (o modelo só escreve a resposta final);
 * "modelo" — o modelo sempre propõe e o roteador valida/corrige. A
 * validação independente é a mesma nos dois: só muda quem fala primeiro. */
static gboolean g_rota_direta = TRUE;

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
    g_free(c->pergunta_modelo_id);
    g_free(c->ultima_pergunta_id);
    g_free(c->ultima_pergunta_texto);
    g_free(c->resumo_pendente_id);
    agente_plano_liberar(c->plano_proposto);
    g_free(c->plano_proposto_pergunta_id);
    g_free(c->plano_proposto_pedido);
    g_free(c->plano_hash_pendente);
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

static gboolean web_automatica(const ExecutorManifesto *mf);
static gboolean requer_aprovacao_efetiva(const ExecutorManifesto *mf);

static void enviar_plano(Cliente *c, const char *em_resposta_a, const ExecutorManifesto *mf)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "plano_id");
    json_builder_add_string_value(b, c->plano_id_pendente);
    /* a aprovação é vinculada a ESTE hash (plano_id + ferramenta +
     * parâmetros normalizados), não só ao plano_id: ver tratar_aprovar */
    json_builder_set_member_name(b, "hash");
    json_builder_add_string_value(b, c->plano_hash_pendente ? c->plano_hash_pendente : "");
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
    json_builder_add_boolean_value(b, requer_aprovacao_efetiva(mf));
    json_builder_set_member_name(b, "web_automatica");
    json_builder_add_boolean_value(b, web_automatica(mf));
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

/* 'apresentacao' (opcional): texto legível derivado SÓ de dados_json por
 * sessiond/apresentacao.c — é o que um cliente mostra à pessoa como "o
 * dado real"; o JSON bruto continua em 'dados', intacto. */
static void enviar_resultado_ex(Cliente *c, const char *estado, int codigo_saida, const char *dados_json,
                                 const char *apresentacao)
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
    if (apresentacao) {
        json_builder_set_member_name(b, "apresentacao");
        json_builder_add_string_value(b, apresentacao);
    }
    if (c->exec_inicio_us) {
        json_builder_set_member_name(b, "tempo_execucao_ms");
        json_builder_add_int_value(b, (g_get_monotonic_time() - c->exec_inicio_us) / 1000);
    }
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_RESULTADO, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void enviar_resultado(Cliente *c, const char *estado, int codigo_saida, const char *dados_json)
{
    enviar_resultado_ex(c, estado, codigo_saida, dados_json, NULL);
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
        "%s uid=%u acao=sessiond.%s risco=%s resultado=%s%s\n",
        ts_linha,
        (unsigned)geteuid(),
        c->ferramenta_pendente ? c->ferramenta_pendente : "?",
        mf ? mf->risco : "?",
        resultado_resumo,
        web_automatica(mf) ? " aprovacao=dispensada_web_automatica" : "");

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
    g_free(c->plano_hash_pendente); c->plano_hash_pendente = NULL;
    g_free(c->ferramenta_pendente); c->ferramenta_pendente = NULL;
    g_free(c->execucao_id_atual); c->execucao_id_atual = NULL;
    if (c->parametros_pendentes) { json_node_unref(c->parametros_pendentes); c->parametros_pendentes = NULL; }
    if (c->timeout_execucao_id) { g_source_remove(c->timeout_execucao_id); c->timeout_execucao_id = 0; }
    g_free(c->objetivo_pendente); c->objetivo_pendente = NULL;
    g_free(c->justificativa_pendente); c->justificativa_pendente = NULL;
    g_free(c->pergunta_id_origem); c->pergunta_id_origem = NULL;
    c->origem_modelo = FALSE;
    c->exec_inicio_us = 0;
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
        gboolean pedir_resumo = FALSE, truncado = FALSE;
        char *apresentacao = NULL, *falha_texto = NULL;
        gint64 exec_ms = c->exec_inicio_us ? (g_get_monotonic_time() - c->exec_inicio_us) / 1000 : 0;

        if (r->sucesso) {
            const char *estado = r->codigo_saida == 0 ? "sucesso" : "falha";
            apresentacao = apresentacao_renderizar(c->ferramenta_pendente, r->dados_json, &truncado);
            enviar_resultado_ex(c, estado, r->codigo_saida, r->dados_json, apresentacao);
            if (!c->fechado) enviar_auditoria(c, estado);
            if (!c->fechado) registrar_auditoria_disco(g_daemon_para_auditoria, c, estado);
            pedir_resumo = c->origem_modelo;
        } else if (foi_cancelado) {
            enviar_resultado(c, "cancelado", r->codigo_saida, NULL);
            if (!c->fechado) enviar_auditoria(c, "cancelado");
            if (!c->fechado) registrar_auditoria_disco(g_daemon_para_auditoria, c, "cancelado");
        } else {
            enviar_erro(c, NULL, r->erro_codigo ? r->erro_codigo : "erro_interno", r->erro_mensagem);
            if (!c->fechado) enviar_resultado(c, "falha", r->codigo_saida, NULL);
            if (!c->fechado) enviar_auditoria(c, "falha");
            if (!c->fechado) registrar_auditoria_disco(g_daemon_para_auditoria, c, "falha");
            if (c->origem_modelo)
                falha_texto = g_strdup_printf("A ferramenta '%s' falhou (%s: %s). Nenhum dado foi coletado, então não há o que resumir.",
                    c->ferramenta_pendente ? c->ferramenta_pendente : "?",
                    r->erro_codigo ? r->erro_codigo : "erro", r->erro_mensagem ? r->erro_mensagem : "sem detalhe");
        }
        g_print("lia-sessiond: execução de %s concluída em %" G_GINT64_FORMAT " ms (%s)\n",
                c->ferramenta_pendente ? c->ferramenta_pendente : "?", exec_ms,
                r->sucesso ? "sucesso" : (foi_cancelado ? "cancelada" : "falha"));

        char *ferramenta_para_resumo = g_strdup(c->ferramenta_pendente);
        char *pergunta_id_para_resumo = g_strdup(c->pergunta_id_origem);
        char *dados_para_resumo = pedir_resumo ? g_strdup(r->dados_json ? r->dados_json : "{}") : NULL;
        resetar_para_conectado(c);
        if (!c->fechado && pedir_resumo)
            disparar_resumo_modelo(c, ferramenta_para_resumo, pergunta_id_para_resumo, apresentacao, dados_para_resumo, truncado);
        else if (!c->fechado && falha_texto && pergunta_id_para_resumo)
            /* execução pedida pelo fluxo agente falhou: fecha o turno com
             * uma resposta final determinística — nunca deixa o cliente
             * esperando um resumo que não virá, nunca inventa conteúdo. */
            enviar_resposta_final(c, pergunta_id_para_resumo, falha_texto, "deterministica", TRUE, NULL, 0);
        g_free(ferramenta_para_resumo);
        g_free(pergunta_id_para_resumo);
        g_free(dados_para_resumo);
        g_free(apresentacao);
        g_free(falha_texto);
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
        char *pergunta_id_origem = c->origem_modelo ? g_strdup(c->pergunta_id_origem) : NULL;
        char *ferramenta = g_strdup(c->ferramenta_pendente);
        resetar_para_conectado(c);
        if (!c->fechado && pergunta_id_origem) {
            /* fluxo agente: o turno sempre fecha com uma 'resposta' — aqui,
             * determinística, dizendo que não há dado nenhum para resumir */
            char *txt = g_strdup_printf("A ferramenta '%s' excedeu o tempo máximo de execução e foi interrompida. Nenhum dado foi coletado.",
                                         ferramenta ? ferramenta : "?");
            enviar_resposta_final(c, pergunta_id_origem, txt, "deterministica", TRUE, NULL, 0);
            g_free(txt);
        }
        g_free(pergunta_id_origem);
        g_free(ferramenta);
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
    c->exec_inicio_us = g_get_monotonic_time();

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
    char *mensagem_original; /* pedido cru da pessoa — só para o roteador classificar; nunca reenviado a lugar nenhum */
    char *prompt_original;   /* prompt da 1ª tentativa — base da única nova tentativa com correção */
    char *esquema_json;      /* esquema da saída estruturada pedida ao modelo */
    int tentativa;           /* 1 ou 2 */
    gint64 inicio_us;
} PerguntaCtx;

static void pergunta_ctx_liberar(PerguntaCtx *pc)
{
    cliente_unref(pc->c);
    g_free(pc->pergunta_id);
    g_free(pc->mensagem_original);
    g_free(pc->prompt_original);
    g_free(pc->esquema_json);
    g_free(pc);
}

/* Tokens gerados: a proposta estruturada é um objeto JSON pequeno; o
 * resumo, até 3 frases. Medido nesta máquina (llama3.2:3b, CPU): ~9
 * tokens/s — cada 100 tokens a menos são ~11 s a menos de espera. */
#define LIA_MODELO_TOKENS_PROPOSTA 160
#define LIA_MODELO_TOKENS_RESUMO   120
/* linhas do texto renderizado que vão para o prompt de resumo: o modelo
 * só precisa do topo para responder em duas frases (cada linha a mais é
 * tempo de avaliação de prompt); a pessoa vê a tabela inteira, e a
 * checagem de fundamentação usa os dados inteiros. */
#define LIA_RESUMO_LINHAS_PARA_O_MODELO 20

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
        g_string_append_printf(out, "- %s: %s%s\n", mf->nome, mf->uso ? mf->uso : mf->efeitos,
                               requer_aprovacao_efetiva(mf) ? " [exige aprovação de uma pessoa]" : "");
    }
    return g_string_free(out, FALSE);
}

/* Esquema JSON da resposta estruturada pedida ao modelo. "ferramenta" é um
 * enum com os nomes do manifesto ATIVO + "nenhuma": com saída restrita por
 * esquema, o modelo nem consegue escrever um nome fora da lista (e mesmo
 * assim o roteador valida de novo, de forma independente). */
static char *construir_esquema_proposta(void)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "object");
    json_builder_set_member_name(b, "properties");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tipo");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "string");
    json_builder_set_member_name(b, "enum");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "proposta");
    json_builder_add_string_value(b, "resposta");
    json_builder_add_string_value(b, "esclarecimento");
    json_builder_end_array(b);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "ferramenta");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "string");
    json_builder_set_member_name(b, "enum");
    json_builder_begin_array(b);
    const char * const *nomes = g_executor->listar_nomes(g_executor);
    for (int i = 0; nomes[i]; i++)
        json_builder_add_string_value(b, nomes[i]);
    json_builder_add_string_value(b, "nenhuma");
    json_builder_end_array(b);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "parametros");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "object");
    json_builder_end_object(b);
    json_builder_set_member_name(b, "justificativa");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "string");
    json_builder_end_object(b);
    json_builder_set_member_name(b, "resposta");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type"); json_builder_add_string_value(b, "string");
    json_builder_end_object(b);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "required");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "tipo");
    json_builder_add_string_value(b, "ferramenta");
    json_builder_add_string_value(b, "parametros");
    json_builder_add_string_value(b, "justificativa");
    json_builder_add_string_value(b, "resposta");
    json_builder_end_array(b);
    json_builder_end_object(b);
    JsonNode *raiz = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, raiz);
    char *texto = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_unref(raiz);
    g_object_unref(b);
    return texto;
}

/* ---- ponte roteador <-> manifesto do executor ativo ---- */

/* mesma política de capacidades de tratar_solicitar: uma ferramenta que o
 * 'solicitar' recusaria por capacidade nem conta como existente aqui. */
static gboolean capacidades_permitidas(const ExecutorManifesto *mf)
{
    if (!g_executor->restringe_a_leitura)
        return TRUE;
    for (int i = 0; mf->capacidades[i]; i++)
        if (strcmp(mf->capacidades[i], "leitura_arquivo") != 0 && strcmp(mf->capacidades[i], "processo") != 0 && strcmp(mf->capacidades[i], "rede") != 0)
            return FALSE;
    return TRUE;
}

static gboolean rot_existe(const char *ferramenta, gpointer dados)
{
    (void)dados;
    const ExecutorManifesto *mf = g_executor->buscar_manifesto(ferramenta);
    return mf != NULL && capacidades_permitidas(mf);
}

static char *rot_validar(const char *ferramenta, const char *parametros_json, gpointer dados)
{
    (void)dados;
    const ExecutorManifesto *mf = g_executor->buscar_manifesto(ferramenta);
    if (!mf)
        return g_strdup("ferramenta desconhecida");
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, parametros_json ? parametros_json : "{}", -1, NULL) ||
        !json_parser_get_root(p) || JSON_NODE_TYPE(json_parser_get_root(p)) != JSON_NODE_OBJECT) {
        g_object_unref(p);
        return g_strdup("parâmetros não são um objeto JSON");
    }
    char *erro = g_executor->validar_parametros(mf, json_node_get_object(json_parser_get_root(p)));
    g_object_unref(p);
    return erro;
}

static const RoteadorManifesto ROT_MANIFESTO = { rot_existe, rot_validar, NULL };

/* Política do piloto automático, calculada pelo DAEMON (o cliente só
 * obedece — lia-chat nunca decide sozinho que algo é "seguro"): só
 * ferramenta declarada puramente de leitura, risco "nenhum", sem
 * exigência de aprovação e só com capacidades de leitura. Qualquer coisa
 * que grave, persista inventário, execute binários, toque hardware ou
 * peça privilégio fica FALSE e continua parando em confirmação. */
/* Web automática (LIA_SESSIOND_WEB_AUTOMATICA=sim, ligada por padrão em
 * chat/rodar_local.sh): web.pesquisar e web.abrir deixam de pedir
 * confirmação. Vale SÓ para ferramentas "web." cuja única capacidade é
 * "rede" (HTTPS externo, sem gravar nada, sem tocar a máquina); escrita,
 * persistência, administração e execução de programas continuam exigindo
 * aprovação explícita. Cada execução fica na auditoria e o cliente recebe
 * "web_automatica": true para sinalizar na tela. */
static gboolean g_web_automatica;

static gboolean web_automatica(const ExecutorManifesto *mf)
{
    if (!g_web_automatica || !mf || !g_str_has_prefix(mf->nome, "web.")) return FALSE;
    for (int i = 0; mf->capacidades[i]; i++)
        if (strcmp(mf->capacidades[i], "rede") != 0) return FALSE;
    return mf->capacidades[0] != NULL;
}

static gboolean requer_aprovacao_efetiva(const ExecutorManifesto *mf)
{
    return mf && mf->requer_aprovacao && !web_automatica(mf);
}

static gboolean execucao_automatica_permitida(const ExecutorManifesto *mf)
{
    if (!mf || !mf->somente_leitura || mf->requer_aprovacao || strcmp(mf->risco, "nenhum") != 0)
        return FALSE;
    for (int i = 0; mf->capacidades[i]; i++)
        if (strcmp(mf->capacidades[i], "leitura_arquivo") != 0 && strcmp(mf->capacidades[i], "processo") != 0 && strcmp(mf->capacidades[i], "rede") != 0)
            return FALSE;
    return TRUE;
}

static const char *veredito_texto(RoteadorVeredito v)
{
    switch (v) {
    case ROTEADOR_ACEITA: return "aceita";
    case ROTEADOR_CORRIGIDA: return "corrigida";
    case ROTEADOR_SEM_FERRAMENTA: return "sem_ferramenta";
    case ROTEADOR_ESCLARECER: return "esclarecer";
    case ROTEADOR_REPETIR: return "repetir";
    case ROTEADOR_REJEITADA: return "rejeitada";
    }
    return "?";
}

/* 'proposta' depois da validação independente. Campos separados, nunca
 * misturados (docs/protocolo.md §10): texto do modelo (resposta_texto),
 * intenção classificada, o que a validação fez, e a proposta final — que
 * é a ÚNICA coisa que um cliente pode transformar em 'solicitar'. */
static void enviar_proposta(Cliente *c, const char *pergunta_id, const ModeloResposta *r,
                             const Rota *rota, const RoteadorDecisao *d, gint64 tempo_ms, int tentativas)
{
    const ExecutorManifesto *mf = d->ferramenta ? g_executor->buscar_manifesto(d->ferramenta) : NULL;
    gboolean substituir_texto = d->veredito == ROTEADOR_SEM_FERRAMENTA || d->veredito == ROTEADOR_ESCLARECER ||
                                d->veredito == ROTEADOR_REJEITADA;
    const char *tipo = d->ferramenta ? "proposta" : (d->veredito == ROTEADOR_ESCLARECER ? "esclarecimento" : "resposta");

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "pergunta_id");
    json_builder_add_string_value(b, pergunta_id);
    json_builder_set_member_name(b, "tipo_resposta");
    json_builder_add_string_value(b, tipo);
    json_builder_set_member_name(b, "resposta_texto");
    /* quando a validação removeu/recusou a proposta, o texto do modelo
     * (que falava de uma ferramenta que não vai rodar) é trocado pela
     * mensagem do validador — nunca uma promessa que não será cumprida. */
    json_builder_add_string_value(b, substituir_texto ? (d->mensagem ? d->mensagem : "")
                                                       : (r->resposta_texto ? r->resposta_texto : ""));
    json_builder_set_member_name(b, "ferramenta_proposta");
    if (d->ferramenta) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "ferramenta");
        json_builder_add_string_value(b, d->ferramenta);
        embutir_json_bruto(b, "parametros", d->parametros_json);
        json_builder_set_member_name(b, "justificativa");
        if (d->veredito == ROTEADOR_CORRIGIDA || tentativas == 0) json_builder_add_string_value(b, d->motivo);
        else if (r->justificativa) json_builder_add_string_value(b, r->justificativa);
        else json_builder_add_null_value(b);
        json_builder_end_object(b);
    } else {
        json_builder_add_null_value(b);
    }
    json_builder_set_member_name(b, "intencao");
    if (rota->intencao) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "id");
        json_builder_add_string_value(b, rota->intencao);
        json_builder_set_member_name(b, "descricao");
        json_builder_add_string_value(b, rota->descricao ? rota->descricao : "");
        json_builder_set_member_name(b, "confianca");
        json_builder_add_string_value(b, rota->confianca == ROTA_CONFIANCA_ALTA ? "alta" :
                                         rota->confianca == ROTA_CONFIANCA_MEDIA ? "media" : "nenhuma");
        json_builder_end_object(b);
    } else {
        json_builder_add_null_value(b);
    }
    json_builder_set_member_name(b, "validacao");
    json_builder_begin_object(b);
    /* tentativas == 0: o modelo não foi consultado (rota direta) */
    json_builder_set_member_name(b, "origem");
    json_builder_add_string_value(b, tentativas == 0 ? "roteador" : (d->veredito == ROTEADOR_ACEITA ? "modelo" : "modelo+roteador"));
    json_builder_set_member_name(b, "veredito");
    json_builder_add_string_value(b, tentativas == 0 && d->ferramenta ? "rota_direta" : veredito_texto(d->veredito));
    json_builder_set_member_name(b, "motivo");
    json_builder_add_string_value(b, d->motivo ? d->motivo : "");
    json_builder_set_member_name(b, "ferramenta_do_modelo");
    if (r->ferramenta_proposta) json_builder_add_string_value(b, r->ferramenta_proposta);
    else json_builder_add_null_value(b);
    json_builder_set_member_name(b, "saida_estruturada");
    json_builder_add_boolean_value(b, r->estruturada);
    json_builder_set_member_name(b, "tentativas");
    json_builder_add_int_value(b, tentativas);
    if (mf) {
        json_builder_set_member_name(b, "risco");
        json_builder_add_string_value(b, mf->risco);
        json_builder_set_member_name(b, "requer_aprovacao");
        json_builder_add_boolean_value(b, requer_aprovacao_efetiva(mf));
        json_builder_set_member_name(b, "web_automatica");
        json_builder_add_boolean_value(b, web_automatica(mf));
        json_builder_set_member_name(b, "somente_leitura");
        json_builder_add_boolean_value(b, mf->somente_leitura);
    }
    json_builder_set_member_name(b, "execucao_automatica_permitida");
    json_builder_add_boolean_value(b, execucao_automatica_permitida(mf));
    json_builder_end_object(b);
    json_builder_set_member_name(b, "tempo_modelo_ms");
    json_builder_add_int_value(b, tempo_ms);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_PROPOSTA, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void enviar_resposta_final(Cliente *c, const char *pergunta_id, const char *texto, const char *origem,
                                   gboolean fundamentada, const char *aviso, gint64 tempo_modelo_ms)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "pergunta_id");
    json_builder_add_string_value(b, pergunta_id);
    json_builder_set_member_name(b, "resposta_texto");
    json_builder_add_string_value(b, texto ? texto : "");
    /* "modelo": texto do modelo, conferido contra os dados reais;
     * "deterministica": escrito pelo daemon a partir dos dados (o resumo
     * do modelo falhou, foi cancelado ou citou valor sem origem). */
    json_builder_set_member_name(b, "origem");
    json_builder_add_string_value(b, origem);
    json_builder_set_member_name(b, "fundamentada");
    json_builder_add_boolean_value(b, fundamentada);
    json_builder_set_member_name(b, "aviso");
    if (aviso) json_builder_add_string_value(b, aviso);
    else json_builder_add_null_value(b);
    json_builder_set_member_name(b, "tempo_modelo_ms");
    json_builder_add_int_value(b, tempo_modelo_ms);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_RESPOSTA, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void perguntar_ao_modelo(PerguntaCtx *pc, const char *prompt);

static void ao_concluido_pergunta(gpointer ctx, const ModeloResposta *r)
{
    PerguntaCtx *pc = ctx;
    Cliente *c = pc->c;
    gboolean ainda_valido = !c->fechado && c->pergunta_pendente_id &&
                             strcmp(pc->pergunta_id, c->pergunta_pendente_id) == 0;
    if (!ainda_valido) { /* cancelada, ou a conexão caiu: nada a enviar */
        pergunta_ctx_liberar(pc);
        return;
    }
    gint64 tempo_ms = (g_get_monotonic_time() - pc->inicio_us) / 1000;

    if (!r->sucesso) {
        g_print("lia-sessiond: pergunta %s: modelo falhou em %" G_GINT64_FORMAT " ms (%s)\n",
                pc->pergunta_id, tempo_ms, r->erro_codigo ? r->erro_codigo : "?");
        enviar_erro(c, NULL, r->erro_codigo ? r->erro_codigo : "modelo_resposta_invalida", r->erro_mensagem);
    } else {
        if (r->plano_json) {
            /* o modelo propôs um plano de VÁRIOS passos: mesma validação
             * integral (ferramentas, capacidades, parâmetros, dependências,
             * limites). Plano inválido não é "consertado": é rejeitado. */
            char *erro = NULL;
            AgentePlano *plano = agente_plano_de_json(r->plano_json, "plano_do_modelo", &erro);
            if (plano && !erro) erro = agente_plano_validar(plano, &AG_MANIFESTO);
            if (erro) {
                g_print("lia-sessiond: pergunta %s: plano do modelo rejeitado (%s)\n", pc->pergunta_id, erro);
                char *txt = g_strdup_printf("O plano proposto pelo modelo foi rejeitado pela validação (%s). Nada foi executado.", erro);
                enviar_proposta_tarefa(c, pc->pergunta_id, NULL, COMPAT_COMPATIVEL, txt, "modelo+roteador", "rejeitada", erro, tempo_ms, pc->tentativa);
                g_free(txt);
                g_free(erro);
                agente_plano_liberar(plano);
            } else {
                if (!*plano->objetivo) { g_free(plano->objetivo); plano->objetivo = g_strdup(pc->mensagem_original); }
                g_print("lia-sessiond: pergunta %s: plano do modelo validado (%u passos, risco=%s, aprovação=%s)\n",
                        pc->pergunta_id, plano->passos->len, plano->risco, plano->requer_aprovacao ? "sim" : "não");
                enviar_proposta_tarefa(c, pc->pergunta_id, plano, COMPAT_COMPATIVEL, r->resposta_texto, "modelo", "plano_validado",
                                       "plano proposto pelo modelo, validado por inteiro antes de qualquer execução", tempo_ms, pc->tentativa);
                guardar_plano_proposto(c, plano, pc->pergunta_id, pc->mensagem_original);
            }
            g_free(c->pergunta_pendente_id);
            c->pergunta_pendente_id = NULL;
            g_free(c->pergunta_modelo_id);
            c->pergunta_modelo_id = NULL;
            pergunta_ctx_liberar(pc);
            return;
        }
        /* pedido -> intenção classificada -> ferramenta candidata ->
         * validação independente. Só o que sair daqui pode virar plano. */
        Rota rota;
        roteador_classificar(pc->mensagem_original, &rota);
        RoteadorDecisao d;
        roteador_decidir(&rota, pc->mensagem_original, r->ferramenta_proposta, r->parametros_json,
                          &ROT_MANIFESTO, pc->tentativa == 1, &d);
        /* resposta vazia (nem texto, nem ferramenta) também é inválida */
        gboolean vazia = d.veredito == ROTEADOR_ACEITA && !d.ferramenta && (!r->resposta_texto || !*r->resposta_texto);
        if (vazia) {
            d.veredito = pc->tentativa == 1 ? ROTEADOR_REPETIR : ROTEADOR_REJEITADA;
            g_free(d.motivo);
            d.motivo = g_strdup("o modelo devolveu uma resposta vazia (sem texto e sem ferramenta)");
            if (d.veredito == ROTEADOR_REJEITADA)
                d.mensagem = g_strdup("O modelo não devolveu uma resposta válida (duas tentativas). Nada foi executado.");
        }

        if (d.veredito == ROTEADOR_REPETIR) {
            /* correção limitada: UMA nova tentativa, com o motivo da
             * rejeição; nada é executado nem proposto enquanto isso. */
            g_print("lia-sessiond: pergunta %s: resposta do modelo inválida em %" G_GINT64_FORMAT " ms (%s) — nova tentativa\n",
                    pc->pergunta_id, tempo_ms, d.motivo);
            char *prompt2 = prompt_acrescentar_correcao(pc->prompt_original, d.motivo);
            roteador_decisao_liberar(&d);
            pc->tentativa = 2;
            perguntar_ao_modelo(pc, prompt2);
            g_free(prompt2);
            return; /* pc segue vivo para a 2ª resposta */
        }

        g_print("lia-sessiond: pergunta %s: proposta em %" G_GINT64_FORMAT " ms (tentativas=%d, intenção=%s, veredito=%s, ferramenta=%s, modelo propôs=%s)\n",
                pc->pergunta_id, tempo_ms, pc->tentativa, rota.intencao ? rota.intencao : "-",
                veredito_texto(d.veredito), d.ferramenta ? d.ferramenta : "-",
                r->ferramenta_proposta ? r->ferramenta_proposta : "-");
        /* guarda o pedido para o resumo final poder citá-lo */
        g_free(c->ultima_pergunta_id);
        c->ultima_pergunta_id = g_strdup(pc->pergunta_id);
        g_free(c->ultima_pergunta_texto);
        c->ultima_pergunta_texto = g_strdup(pc->mensagem_original);
        enviar_proposta(c, pc->pergunta_id, r, &rota, &d, tempo_ms, pc->tentativa);
        roteador_decisao_liberar(&d);
    }
    g_free(c->pergunta_pendente_id);
    c->pergunta_pendente_id = NULL;
    g_free(c->pergunta_modelo_id);
    c->pergunta_modelo_id = NULL;
    pergunta_ctx_liberar(pc);
}

static void perguntar_ao_modelo(PerguntaCtx *pc, const char *prompt)
{
    /* id próprio por tentativa: os backends tiram a requisição da tabela
     * deles DEPOIS de chamar o callback — reutilizar o mesmo id numa nova
     * tentativa disparada de dentro do callback apagaria a nova entrada. */
    g_free(pc->c->pergunta_modelo_id);
    pc->c->pergunta_modelo_id = g_strdup_printf("%s-t%d", pc->pergunta_id, pc->tentativa);
    ModeloOpcoes op = { .esquema_json = pc->esquema_json, .max_tokens = LIA_MODELO_TOKENS_PROPOSTA, .resumo = FALSE };
    char *id = g_strdup(pc->c->pergunta_modelo_id); /* o callback pode ser síncrono e liberar pergunta_modelo_id */
    g_modelo->perguntar(g_modelo, id, prompt, &op, ao_concluido_pergunta, pc);
    g_free(id);
}

/* ---- resumo final ---- */

typedef struct {
    Cliente *c;
    char *pergunta_id;   /* da pergunta ORIGINAL — é o que o cliente usa para correlacionar */
    char *modelo_id;
    char *ferramenta;
    char *apresentacao;
    char *dados_json;
    char *pedido;
    gboolean truncado;
    gboolean extrativa; /* o modelo foi consultado só para escolher linhas (RESPOSTA_EXTRATIVA) */
    gint64 inicio_us;
} ResumoCtx;

static void resumo_ctx_liberar(ResumoCtx *rc)
{
    cliente_unref(rc->c);
    g_free(rc->pergunta_id);
    g_free(rc->modelo_id);
    g_free(rc->ferramenta);
    g_free(rc->apresentacao);
    g_free(rc->dados_json);
    g_free(rc->pedido);
    g_free(rc);
}

static void ao_concluido_resumo(gpointer ctx, const ModeloResposta *r)
{
    ResumoCtx *rc = ctx;
    Cliente *c = rc->c;
    gboolean ainda_valido = !c->fechado && c->resumo_pendente_id && strcmp(c->resumo_pendente_id, rc->modelo_id) == 0;
    if (ainda_valido) {
        g_free(c->resumo_pendente_id);
        c->resumo_pendente_id = NULL;
        gint64 tempo_ms = (g_get_monotonic_time() - rc->inicio_us) / 1000;
        const char *aviso_truncado = rc->truncado
            ? "a saída da ferramenta passou do limite e foi truncada: a lista acima está incompleta" : NULL;
        char *suspeito = NULL, *fora_de_ordem = NULL;
        const char *evidencias[] = { rc->apresentacao, rc->dados_json, rc->pedido, NULL };
        if (rc->extrativa) {
            /* O modelo só escolheu linhas ({"linhas":[...]}); o texto vem
             * inteiro dos dados reais. Resposta do modelo ausente, inválida
             * ou com índices fora da faixa não quebra nada: sobram as
             * linhas que casam com o que foi perguntado (ou todas). */
            int escolhidas[32], n = 0, validas = 0;
            if (r->sucesso && r->resposta_texto) {
                JsonParser *p = json_parser_new();
                if (json_parser_load_from_data(p, r->resposta_texto, -1, NULL) && json_parser_get_root(p) &&
                    JSON_NODE_TYPE(json_parser_get_root(p)) == JSON_NODE_OBJECT) {
                    JsonObject *o = json_node_get_object(json_parser_get_root(p));
                    JsonNode *nl = json_object_has_member(o, "linhas") ? json_object_get_member(o, "linhas") : NULL;
                    if (nl && JSON_NODE_TYPE(nl) == JSON_NODE_ARRAY) {
                        JsonArray *arr = json_node_get_array(nl);
                        for (guint i = 0; i < json_array_get_length(arr) && n < 32; i++) {
                            JsonNode *el = json_array_get_element(arr, i);
                            if (JSON_NODE_TYPE(el) == JSON_NODE_VALUE && json_node_get_value_type(el) == G_TYPE_INT64)
                                escolhidas[n++] = (int)CLAMP(json_node_get_int(el), -1, 100000);
                        }
                    }
                }
                g_object_unref(p);
            }
            char *texto = apresentacao_resposta_extrativa(rc->apresentacao, rc->pedido, escolhidas, n, &validas);
            char *aviso = NULL;
            if (!r->sucesso)
                aviso = g_strdup_printf("o modelo não respondeu à seleção de linhas (%s) — usadas as linhas que casam com a pergunta",
                                         r->erro_codigo ? r->erro_codigo : "erro");
            else if (validas == 0)
                aviso = g_strdup("a seleção de linhas do modelo veio vazia ou inválida — usadas as linhas que casam com a pergunta");
            g_print("lia-sessiond: pergunta %s: resposta extrativa em %" G_GINT64_FORMAT " ms (%d linha(s) válida(s) escolhida(s) pelo modelo)\n",
                    rc->pergunta_id, tempo_ms, validas);
            enviar_resposta_final(c, rc->pergunta_id, texto, validas > 0 ? "modelo_extrativa" : "deterministica", TRUE,
                                   aviso ? aviso : aviso_truncado, tempo_ms);
            g_free(texto);
            g_free(aviso);
        } else if (r->sucesso && r->resposta_texto && *r->resposta_texto &&
            !apresentacao_resumo_e_recusa(r->resposta_texto) &&
            apresentacao_resumo_fundamentado(r->resposta_texto, evidencias, &suspeito) &&
            apresentacao_resumo_respeita_ranking(r->resposta_texto, rc->ferramenta, rc->dados_json, &fora_de_ordem)) {
            g_print("lia-sessiond: pergunta %s: resumo do modelo em %" G_GINT64_FORMAT " ms (fundamentado nos dados)\n",
                    rc->pergunta_id, tempo_ms);
            enviar_resposta_final(c, rc->pergunta_id, r->resposta_texto, "modelo", TRUE, aviso_truncado, tempo_ms);
        } else {
            /* O resumo do modelo não é usado: ou não veio, ou cita um
             * número que a ferramenta não devolveu. A resposta final passa
             * a ser determinística — os dados reais já foram entregues em
             * 'resultado' — e diz por quê. Nunca um valor inventado. */
            char *aviso;
            if (!r->sucesso)
                aviso = g_strdup_printf("o modelo não gerou o resumo (%s: %s)%s%s",
                    r->erro_codigo ? r->erro_codigo : "erro", r->erro_mensagem ? r->erro_mensagem : "sem detalhe",
                    aviso_truncado ? "; " : "", aviso_truncado ? aviso_truncado : "");
            else if (suspeito)
                aviso = g_strdup_printf("o resumo do modelo foi descartado: citava o número \"%s\", que não está nos dados coletados%s%s",
                    suspeito, aviso_truncado ? "; " : "", aviso_truncado ? aviso_truncado : "");
            else if (fora_de_ordem)
                aviso = g_strdup_printf("o resumo do modelo foi descartado: %s%s%s",
                    fora_de_ordem, aviso_truncado ? "; " : "", aviso_truncado ? aviso_truncado : "");
            else if (r->resposta_texto && *r->resposta_texto)
                aviso = g_strdup_printf("o resumo do modelo foi descartado: negava ter os dados que a ferramenta devolveu%s%s",
                    aviso_truncado ? "; " : "", aviso_truncado ? aviso_truncado : "");
            else
                aviso = g_strdup_printf("o modelo devolveu um resumo vazio%s%s",
                    aviso_truncado ? "; " : "", aviso_truncado ? aviso_truncado : "");
            char *texto = apresentacao_frase(rc->ferramenta, rc->dados_json);
            g_print("lia-sessiond: pergunta %s: resumo determinístico após %" G_GINT64_FORMAT " ms (%s)\n",
                    rc->pergunta_id, tempo_ms, !r->sucesso ? "modelo falhou" :
                    (suspeito ? "resumo citava número sem origem — descartado" : (fora_de_ordem ? "resumo pulava colocados do ranking — descartado" : "resumo vazio")));
            enviar_resposta_final(c, rc->pergunta_id, texto, "deterministica", TRUE, aviso, tempo_ms);
            g_free(texto);
            g_free(aviso);
        }
        g_free(suspeito);
        g_free(fora_de_ordem);
    }
    resumo_ctx_liberar(rc);
}

/* Disparado depois que o 'resultado' de uma execução com origem_modelo foi
 * enviado: pede ao modelo SOMENTE um resumo dos dados reais (prompt
 * próprio, curto — sessiond/prompt.c, prompt_montar_resumo). O modelo
 * nunca propõe/aprova/executa nada aqui (ModeloOpcoes.resumo). O turno
 * SEMPRE termina com uma 'resposta': a do modelo, se fundamentada; senão
 * a determinística (ver ao_concluido_resumo). */
static void disparar_resumo_modelo(Cliente *c, const char *ferramenta, const char *pergunta_id_origem,
                                    const char *apresentacao, const char *dados_json, gboolean truncado)
{
    if (!pergunta_id_origem)
        return;
    RespostaModo modo = apresentacao_modo_resposta(ferramenta);
    if (modo == RESPOSTA_DETERMINISTICA || !g_modelo) {
        /* ranking/lista: a resposta sai dos próprios dados, na hora, sem
         * consultar modelo nenhum — não há o que um modelo acrescentar a
         * "quem é o primeiro e quanto usa" além de risco de erro. */
        char *texto = apresentacao_frase(ferramenta, dados_json);
        g_print("lia-sessiond: pergunta %s: resposta determinística (ferramenta de ranking/lista; modelo não consultado)\n", pergunta_id_origem);
        enviar_resposta_final(c, pergunta_id_origem, texto, "deterministica", TRUE,
                               truncado ? "a saída da ferramenta passou do limite e foi truncada: a lista acima está incompleta" : NULL, 0);
        g_free(texto);
        return;
    }
    ResumoCtx *rc = g_new0(ResumoCtx, 1);
    rc->c = cliente_ref(c);
    rc->pergunta_id = g_strdup(pergunta_id_origem);
    rc->modelo_id = g_strdup_printf("%s-resumo", pergunta_id_origem);
    rc->ferramenta = g_strdup(ferramenta);
    rc->apresentacao = g_strdup(apresentacao ? apresentacao : "");
    rc->dados_json = g_strdup(dados_json ? dados_json : "{}");
    /* o pedido original só é citado se for mesmo o desta pergunta */
    rc->pedido = g_strdup(c->ultima_pergunta_id && strcmp(c->ultima_pergunta_id, pergunta_id_origem) == 0 &&
                           c->ultima_pergunta_texto ? c->ultima_pergunta_texto : "");
    rc->truncado = truncado;
    rc->extrativa = modo == RESPOSTA_EXTRATIVA;
    rc->inicio_us = g_get_monotonic_time();

    if (rc->extrativa) {
        char *numeradas = apresentacao_numerar_linhas(rc->apresentacao, NULL);
        char *prompt_sel = prompt_montar_selecao(rc->pedido, numeradas);
        g_free(numeradas);
        g_free(c->resumo_pendente_id);
        c->resumo_pendente_id = g_strdup(rc->modelo_id);
        ModeloOpcoes ops = {
            .esquema_json = "{\"type\":\"object\",\"properties\":{\"linhas\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}},\"required\":[\"linhas\"]}",
            .max_tokens = 40, .resumo = TRUE };
        char *ids = g_strdup(rc->modelo_id);
        g_modelo->perguntar(g_modelo, ids, prompt_sel, &ops, ao_concluido_resumo, rc);
        g_free(ids);
        g_free(prompt_sel);
        return;
    }

    /* versão enxuta dos dados para o prompt (ver LIA_RESUMO_LINHAS_PARA_O_MODELO) */
    GString *enxuto = g_string_new(NULL);
    int linhas = 0, restantes = 0;
    for (const char *p = rc->apresentacao; *p;) {
        const char *nl = strchr(p, '\n');
        gsize tam = nl ? (gsize)(nl - p) + 1 : strlen(p);
        if (linhas < LIA_RESUMO_LINHAS_PARA_O_MODELO) g_string_append_len(enxuto, p, (gssize)tam);
        else restantes++;
        linhas++;
        p += tam;
    }
    if (restantes)
        g_string_append_printf(enxuto, "[... mais %d linhas foram mostradas à pessoa e não estão aqui; não fale delas]\n", restantes);
    char *prompt_final = prompt_montar_resumo(rc->pedido, ferramenta, enxuto->str);
    g_string_free(enxuto, TRUE);
    g_free(c->resumo_pendente_id);
    c->resumo_pendente_id = g_strdup(rc->modelo_id);
    ModeloOpcoes op = { .esquema_json = NULL, .max_tokens = LIA_MODELO_TOKENS_RESUMO, .resumo = TRUE };
    char *id = g_strdup(rc->modelo_id);
    g_modelo->perguntar(g_modelo, id, prompt_final, &op, ao_concluido_resumo, rc);
    g_free(id);
    g_free(prompt_final);
}

static void tratar_perguntar(Cliente *c, const LiaMsg *m)
{
    if (c->pergunta_pendente_id) {
        enviar_erro(c, m->id, "estado_invalido", "já existe uma pergunta em andamento nesta conexão");
        return;
    }
    const char *mensagem = campo_string(m->dados, "mensagem");
    const char *contexto_conversa = campo_string(m->dados, "contexto");
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
    PromptErro pe = prompt_montar_com_contexto(manifesto_texto, mensagem, contexto_conversa, arquivos, &prompt_final);
    g_free(manifesto_texto);
    g_list_free_full(arquivos, (GDestroyNotify)prompt_arquivo_liberar);

    if (pe != PROMPT_OK) {
        enviar_erro(c, m->id, "parametro_invalido", prompt_erro_texto(pe));
        g_free(prompt_final);
        return;
    }

    /* Agent Runtime v2: o pedido é uma TAREFA de vários passos (lentidão,
     * comparação, relatório, inventário, evidências sobre malware...)? O
     * planejador é determinístico; o plano é validado inteiro aqui e só
     * então proposto. Pergunta para a qual não existe ferramenta (ex.:
     * "tem vírus?") recebe a resposta direta — nunca uma ferramenta
     * "parecida" apresentada como se respondesse. */
    {
        char *texto_direto = NULL;
        AgenteCompat compat = COMPAT_COMPATIVEL;
        AgentePlano *plano = agente_planejar_com_contexto(mensagem, contexto_conversa, &texto_direto, &compat);
        if (plano || texto_direto) {
            char *pid = lia_novo_id("perg");
            char *erro = plano ? agente_plano_validar(plano, &AG_MANIFESTO) : NULL;
            if (plano && erro && g_str_has_prefix(erro, "ferramenta_desconhecida")) {
                /* executor ativo sem as ferramentas do plano (ex.: mock): a
                 * tarefa não se aplica aqui — segue o fluxo de consulta simples */
                g_free(erro);
                g_free(pid);
                agente_plano_liberar(plano);
            } else {
                if (erro) {
                    g_print("lia-sessiond: pergunta %s: plano de tarefa inválido (%s)\n", pid, erro);
                    char *txt = g_strdup_printf("Não foi possível montar um plano válido para este pedido (%s). Nada foi executado.", erro);
                    enviar_proposta_tarefa(c, pid, NULL, COMPAT_INEXISTENTE, txt, "roteador", "rejeitada", erro, 0, 0);
                    g_free(txt);
                    g_free(erro);
                    agente_plano_liberar(plano);
                } else if (plano) {
                    g_print("lia-sessiond: pergunta %s: tarefa planejada (intenção=%s, %u passos, compatibilidade=%s, hash=%.12s…)\n",
                            pid, plano->intencao, plano->passos->len, agente_compat_texto(compat), plano->hash);
                    enviar_proposta_tarefa(c, pid, plano, compat, "", "roteador", "plano_validado",
                                           "plano determinístico de vários passos, validado por inteiro antes de qualquer execução", 0, 0);
                    guardar_plano_proposto(c, plano, pid, mensagem);
                } else {
                    g_print("lia-sessiond: pergunta %s: sem ferramenta para o pedido (compatibilidade=%s) — resposta direta, nada executado\n",
                            pid, agente_compat_texto(compat));
                    enviar_proposta_tarefa(c, pid, NULL, compat, texto_direto, "roteador", "sem_ferramenta",
                                           "nenhuma ferramenta desta versão responde a este pedido; nenhuma ferramenta parecida foi usada no lugar", 0, 0);
                }
                g_free(texto_direto);
                g_free(pid);
                g_free(prompt_final);
                return;
            }
            g_free(texto_direto);
        }
    }

    /* Rota direta: pedido -> intenção de alta confiança -> ferramenta. O
     * modelo local leva dezenas de segundos só para escolher uma ferramenta
     * que a classificação determinística já conhece (medido: 41 s) — e,
     * quando erra, é ela mesma que corrige. Aqui a proposta sai na hora,
     * pela MESMA função de decisão do caminho com modelo (roteador_decidir
     * com "modelo não propôs nada"); nada muda no que pode ou não executar. */
    if (g_rota_direta) {
        Rota rota;
        roteador_classificar(mensagem, &rota);
        gboolean direta = rota.intencao && rota.confianca == ROTA_CONFIANCA_ALTA &&
            (rota.sem_ferramenta || (rota.ferramenta && !rota.esclarecer && rot_existe(rota.ferramenta, NULL)));
        if (direta) {
            RoteadorDecisao d;
            roteador_decidir(&rota, mensagem, NULL, NULL, &ROT_MANIFESTO, FALSE, &d);
            if (d.ferramenta) {
                g_free(d.motivo);
                d.motivo = g_strdup_printf("rota direta: intenção '%s' (confiança alta) -> '%s'; o modelo não foi consultado para escolher a ferramenta",
                                            rota.intencao, d.ferramenta);
            }
            char *pid = lia_novo_id("perg");
            g_print("lia-sessiond: pergunta %s: rota direta (intenção=%s, ferramenta=%s) — modelo não consultado para a proposta\n",
                    pid, rota.intencao, d.ferramenta ? d.ferramenta : "-");
            g_free(c->ultima_pergunta_id);
            c->ultima_pergunta_id = g_strdup(pid);
            g_free(c->ultima_pergunta_texto);
            c->ultima_pergunta_texto = g_strdup(mensagem);
            ModeloResposta vazio = { 0 };
            vazio.sucesso = TRUE;
            enviar_proposta(c, pid, &vazio, &rota, &d, 0, 0);
            roteador_decisao_liberar(&d);
            g_free(pid);
            g_free(prompt_final);
            return;
        }
    }

    char *pergunta_id = lia_novo_id("perg");
    c->pergunta_pendente_id = g_strdup(pergunta_id);
    PerguntaCtx *pc = g_new0(PerguntaCtx, 1);
    pc->c = cliente_ref(c);
    pc->pergunta_id = pergunta_id;
    pc->mensagem_original = g_strdup(mensagem);
    pc->prompt_original = prompt_final;
    pc->esquema_json = construir_esquema_proposta();
    pc->tentativa = 1;
    pc->inicio_us = g_get_monotonic_time();
    perguntar_ao_modelo(pc, pc->prompt_original);
}


/* =================================================== Agent Runtime v2 === */
/* Máquina de execução de uma TAREFA (plano de vários passos). A parte pura
 * (validação, ordem, evidências, verificador, replanejador, relatório) é
 * sessiond/agente.c; aqui ficam o que tem estado e tempo: despacho ao
 * executor, temporizadores, cancelamento e as mensagens do protocolo. */

typedef enum {
    TAREFA_PLANO_RECEBIDO = 0,
    TAREFA_VALIDANDO,
    TAREFA_AGUARDANDO_APROVACAO,
    TAREFA_EXECUTANDO,
    TAREFA_OBSERVANDO,
    TAREFA_VERIFICANDO,
    TAREFA_REPLANEJANDO,
    TAREFA_CONCLUIDO,
    TAREFA_FALHA,
    TAREFA_CANCELADO,
} TarefaEstado;

static const char *tarefa_estado_texto(TarefaEstado e)
{
    static const char *const N[] = { "PLANO_RECEBIDO", "VALIDANDO", "AGUARDANDO_APROVACAO", "EXECUTANDO", "OBSERVANDO",
                                      "VERIFICANDO", "REPLANEJANDO", "CONCLUIDO", "FALHA", "CANCELADO" };
    return N[e];
}

struct Tarefa {
    guint refs;
    Cliente *c;               /* referência contada */
    AgentePlano *plano;
    TarefaEstado estado;
    char *exec_id;            /* execucao_id da tarefa inteira (progresso/resultado/cancelar) */
    char *pergunta_id;        /* pergunta que originou, ou NULL (plano enviado pelo cliente) */
    char *pedido;             /* texto original da pessoa, ou NULL */
    gint64 inicio_us;
    guint timer_global;
    gboolean encerrada;       /* TRUE depois do resultado final: callbacks atrasados não fazem mais nada */
    char *exec_passo[AGENTE_MAX_PASSOS];
    guint timer_passo[AGENTE_MAX_PASSOS];
    gint64 inicio_passo[AGENTE_MAX_PASSOS];
    gboolean sugestao_pedida; /* o modelo já foi consultado para um passo de replanejamento nesta tarefa */
    char *modelo_id;          /* consulta ao modelo em andamento (sugestão de replanejamento), para cancelar */
};

static Tarefa *tarefa_ref(Tarefa *t) { t->refs++; return t; }

static void tarefa_unref(Tarefa *t)
{
    if (--t->refs > 0) return;
    agente_plano_liberar(t->plano);
    g_free(t->exec_id);
    g_free(t->pergunta_id);
    g_free(t->pedido);
    g_free(t->modelo_id);
    for (int i = 0; i < AGENTE_MAX_PASSOS; i++) g_free(t->exec_passo[i]);
    cliente_unref(t->c);
    g_free(t);
}

/* TRUE quando o plano roda sem confirmação SÓ porque todos os seus passos
 * são web automática (o cliente sinaliza isso na tela) */
static gboolean plano_web_automatico(const AgentePlano *plano)
{
    if (!plano || plano->requer_aprovacao || plano->passos->len == 0) return FALSE;
    for (guint i = 0; i < plano->passos->len; i++) {
        const AgentePasso *s = g_ptr_array_index(plano->passos, i);
        if (!web_automatica(g_executor->buscar_manifesto(s->ferramenta))) return FALSE;
    }
    return TRUE;
}

/* ---- ponte agente <-> manifesto do executor ativo ---- */
static gboolean ag_info(const char *ferramenta, AgenteFerramentaInfo *out, gpointer dados)
{
    (void)dados;
    const ExecutorManifesto *mf = g_executor->buscar_manifesto(ferramenta);
    if (!mf) return FALSE;
    out->risco = mf->risco;
    out->requer_aprovacao = requer_aprovacao_efetiva(mf);
    out->somente_leitura = execucao_automatica_permitida(mf);
    out->capacidade_permitida = capacidades_permitidas(mf);
    out->capacidades = mf->capacidades;
    out->timeout_padrao_ms = mf->timeout_padrao_ms;
    return TRUE;
}

static const AgenteManifesto AG_MANIFESTO = { ag_info, rot_validar, NULL };

static void auditar_passo(Cliente *c, const AgentePasso *s, const char *resumo)
{
    const ExecutorManifesto *mf = g_executor->buscar_manifesto(s->ferramenta);
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
    json_builder_add_string_value(b, s->ferramenta);
    json_builder_set_member_name(b, "passo_id");
    json_builder_add_string_value(b, s->id);
    embutir_json_bruto(b, "parametros_normalizados", s->parametros_json);
    json_builder_set_member_name(b, "capacidades");
    json_builder_begin_array(b);
    if (mf)
        for (int i = 0; mf->capacidades[i]; i++)
            json_builder_add_string_value(b, mf->capacidades[i]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "aprovacao");
    json_builder_add_string_value(b, web_automatica(mf) ? "dispensada: web automática (LIA_SESSIOND_WEB_AUTOMATICA=sim)"
                                     : requer_aprovacao_efetiva(mf) ? "aprovada por uma pessoa" : "não exigida (leitura local)");
    json_builder_set_member_name(b, "resultado_resumo");
    json_builder_add_string_value(b, resumo);
    json_builder_set_member_name(b, "reversao_disponivel");
    json_builder_add_boolean_value(b, mf && strcmp(mf->reversivel, "sim") == 0);
    json_builder_end_object(b);
    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_AUDITORIA, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);

    /* disco: mesma linha de sempre (sem parâmetros/conteúdo) — um registro por passo */
    Daemon *d = g_daemon_para_auditoria;
    if (!d || (g_mkdir_with_parents(d->caminho_dir_auditoria, 0700) != 0 && errno != EEXIST))
        return;
    GDateTime *agora = g_date_time_new_now_utc();
    char *data = g_date_time_format(agora, "%Y-%m-%d");
    char *caminho = g_strdup_printf("%s/%s.log", d->caminho_dir_auditoria, data);
    char *ts_linha = g_date_time_format(agora, "%Y-%m-%d %H:%M:%S UTC");
    char *texto = g_strdup_printf("%s uid=%u acao=sessiond.%s risco=%s resultado=%s tarefa_passo=%s%s\n",
                                   ts_linha, (unsigned)geteuid(), s->ferramenta, mf ? mf->risco : "?", resumo, s->id,
                                   web_automatica(mf) ? " aprovacao=dispensada_web_automatica" : "");
    int fd = open(caminho, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ssize_t n = write(fd, texto, strlen(texto));
        (void)n;
        close(fd);
    }
    g_free(texto);
    g_free(ts_linha);
    g_free(caminho);
    g_free(data);
    g_date_time_unref(agora);
}

/* 'progresso' de tarefa: fase atual e, quando for sobre um passo, a
 * observação dele (estado, duração, evidências, limitações, erro e o
 * texto legível dos dados). 'com_plano' anexa o plano (replanejamento). */
static void enviar_progresso_tarefa(Tarefa *t, const char *fase, int idx, const char *mensagem, gboolean com_plano)
{
    Cliente *c = t->c;
    if (c->fechado) return;
    guint total = t->plano->passos->len, feitos = 0;
    for (guint i = 0; i < total; i++) {
        PassoEstado e = ((AgentePasso *)g_ptr_array_index(t->plano->passos, i))->estado;
        if (e != PASSO_AGUARDANDO && e != PASSO_EXECUTANDO) feitos++;
    }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "execucao_id");
    json_builder_add_string_value(b, t->exec_id);
    json_builder_set_member_name(b, "fracao");
    json_builder_add_double_value(b, total ? (double)feitos / (double)total : 0.0);
    json_builder_set_member_name(b, "mensagem");
    json_builder_add_string_value(b, mensagem ? mensagem : "");
    json_builder_set_member_name(b, "tarefa");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "plano_id");
    json_builder_add_string_value(b, t->plano->plano_id);
    json_builder_set_member_name(b, "estado");
    json_builder_add_string_value(b, tarefa_estado_texto(t->estado));
    json_builder_set_member_name(b, "fase");
    json_builder_add_string_value(b, fase);
    json_builder_set_member_name(b, "passos_total");
    json_builder_add_int_value(b, total);
    json_builder_set_member_name(b, "passos_concluidos");
    json_builder_add_int_value(b, feitos);
    json_builder_set_member_name(b, "replanejamentos");
    json_builder_add_int_value(b, t->plano->replanejamentos);
    json_builder_set_member_name(b, "tempo_decorrido_ms");
    json_builder_add_int_value(b, (g_get_monotonic_time() - t->inicio_us) / 1000);
    if (idx >= 0) {
        AgentePasso *s = g_ptr_array_index(t->plano->passos, (guint)idx);
        json_builder_set_member_name(b, "passo");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "indice"); json_builder_add_int_value(b, idx + 1);
        json_builder_set_member_name(b, "id"); json_builder_add_string_value(b, s->id);
        json_builder_set_member_name(b, "ferramenta"); json_builder_add_string_value(b, s->ferramenta);
        embutir_json_bruto(b, "parametros", s->parametros_json);
        json_builder_set_member_name(b, "objetivo"); json_builder_add_string_value(b, s->objetivo);
        json_builder_set_member_name(b, "estado"); json_builder_add_string_value(b, agente_passo_estado_texto(s->estado));
        json_builder_set_member_name(b, "de_replanejamento"); json_builder_add_boolean_value(b, s->de_replanejamento);
        json_builder_set_member_name(b, "tempo_execucao_ms"); json_builder_add_int_value(b, s->duracao_ms);
        json_builder_set_member_name(b, "evidencias");
        json_builder_begin_array(b);
        for (int i = 0; s->evidencias && s->evidencias[i]; i++) json_builder_add_string_value(b, s->evidencias[i]);
        json_builder_end_array(b);
        json_builder_set_member_name(b, "limitacoes");
        json_builder_begin_array(b);
        for (int i = 0; s->limitacoes && s->limitacoes[i]; i++) json_builder_add_string_value(b, s->limitacoes[i]);
        json_builder_end_array(b);
        json_builder_set_member_name(b, "erro");
        if (s->erro) json_builder_add_string_value(b, s->erro); else json_builder_add_null_value(b);
        if (s->apresentacao && s->estado == PASSO_SUCESSO) {
            json_builder_set_member_name(b, "apresentacao");
            json_builder_add_string_value(b, s->apresentacao);
        }
        json_builder_end_object(b);
    }
    if (com_plano) {
        char *pj = agente_plano_para_json(t->plano, TRUE);
        embutir_json_bruto(b, "plano", pj);
        g_free(pj);
    }
    json_builder_end_object(b);
    json_builder_end_object(b);
    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_PROGRESSO, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

typedef struct {
    Tarefa *t;
    int idx;
    char *exec_id;
} PassoCtx;

static PassoCtx *passo_ctx_novo(Tarefa *t, int idx)
{
    PassoCtx *pc = g_new0(PassoCtx, 1);
    pc->t = tarefa_ref(t);
    pc->idx = idx;
    pc->exec_id = g_strdup(t->exec_passo[idx]);
    return pc;
}

static void passo_ctx_liberar(gpointer dados)
{
    PassoCtx *pc = dados;
    tarefa_unref(pc->t);
    g_free(pc->exec_id);
    g_free(pc);
}

static gboolean passo_ctx_atual(PassoCtx *pc)
{
    Tarefa *t = pc->t;
    AgentePasso *s = g_ptr_array_index(t->plano->passos, (guint)pc->idx);
    return !t->encerrada && s->estado == PASSO_EXECUTANDO && t->exec_passo[pc->idx] &&
           strcmp(t->exec_passo[pc->idx], pc->exec_id) == 0;
}

static void tarefa_avancar(Tarefa *t);
static void tarefa_finalizar(Tarefa *t, AgenteVeredito v, char **ausentes, const char *motivo_extra);

static void ao_passo_saida(gpointer ctx, const char *trecho) { (void)ctx; (void)trecho; }
static void ao_passo_progresso(gpointer ctx, double fracao, const char *mensagem) { (void)ctx; (void)fracao; (void)mensagem; }

/* OBSERVAÇÃO: o resultado de um passo vira dados + texto legível +
 * evidências conferidas + limitações. Nada aqui é interpretado como
 * instrução: é só dado guardado no passo. */
static void ao_passo_concluido(gpointer ctx, const ExecutorResultado *r)
{
    PassoCtx *pc = ctx;
    Tarefa *t = pc->t;
    if (!passo_ctx_atual(pc)) { /* passo já resolvido por timeout/cancelamento, ou tarefa encerrada */
        passo_ctx_liberar(pc);
        return;
    }
    int idx = pc->idx;
    AgentePasso *s = g_ptr_array_index(t->plano->passos, (guint)idx);
    if (t->timer_passo[idx]) { g_source_remove(t->timer_passo[idx]); t->timer_passo[idx] = 0; }
    s->duracao_ms = (g_get_monotonic_time() - t->inicio_passo[idx]) / 1000;
    t->estado = TAREFA_OBSERVANDO;
    if (r->sucesso && r->codigo_saida == 0) {
        gboolean truncado = FALSE;
        s->estado = PASSO_SUCESSO;
        s->dados_json = g_strdup(r->dados_json ? r->dados_json : "{}");
        s->apresentacao = apresentacao_renderizar(s->ferramenta, s->dados_json, &truncado);
        g_strfreev(s->evidencias);
        g_strfreev(s->limitacoes);
        s->evidencias = agente_extrair_evidencias(s->ferramenta, s->parametros_json, s->dados_json, &s->limitacoes);
        if (truncado) {
            guint n = g_strv_length(s->limitacoes);
            s->limitacoes = g_realloc(s->limitacoes, sizeof(char *) * (n + 2));
            s->limitacoes[n] = g_strdup("a saída passou do limite e foi truncada: a listagem mostrada está incompleta");
            s->limitacoes[n + 1] = NULL;
        }
    } else if (r->erro_codigo && strcmp(r->erro_codigo, "cancelado") == 0) {
        s->estado = PASSO_CANCELADO;
        s->erro = g_strdup("cancelado");
    } else {
        s->estado = PASSO_FALHA;
        s->erro = r->sucesso ? g_strdup_printf("a ferramenta terminou com código de saída %d", r->codigo_saida)
                             : g_strdup_printf("%s: %s", r->erro_codigo ? r->erro_codigo : "erro", r->erro_mensagem ? r->erro_mensagem : "sem detalhe");
    }
    g_print("lia-sessiond: tarefa %s: passo %d/%u (%s) %s em %" G_GINT64_FORMAT " ms\n", t->plano->plano_id, idx + 1,
            t->plano->passos->len, s->ferramenta, agente_passo_estado_texto(s->estado), s->duracao_ms);
    auditar_passo(t->c, s, agente_passo_estado_texto(s->estado));
    char *msg = g_strdup_printf("passo %d/%u %s: %s", idx + 1, t->plano->passos->len, s->ferramenta, agente_passo_estado_texto(s->estado));
    enviar_progresso_tarefa(t, "observando", idx, msg, FALSE);
    g_free(msg);
    tarefa_ref(t);
    passo_ctx_liberar(pc);
    tarefa_avancar(t);
    tarefa_unref(t);
}

static gboolean ao_timeout_passo(gpointer ctx)
{
    PassoCtx *pc = ctx;
    Tarefa *t = pc->t;
    if (passo_ctx_atual(pc)) {
        int idx = pc->idx;
        AgentePasso *s = g_ptr_array_index(t->plano->passos, (guint)idx);
        t->timer_passo[idx] = 0;
        s->estado = PASSO_FALHA; /* antes de pedir o cancelamento: o callback do executor pode vir síncrono */
        s->duracao_ms = (g_get_monotonic_time() - t->inicio_passo[idx]) / 1000;
        s->erro = g_strdup_printf("limite_excedido: o passo passou do tempo máximo de %d ms", s->timeout_ms);
        g_print("lia-sessiond: tarefa %s: passo %d (%s) excedeu %d ms\n", t->plano->plano_id, idx + 1, s->ferramenta, s->timeout_ms);
        auditar_passo(t->c, s, "limite_excedido");
        char *msg = g_strdup_printf("passo %d/%u %s: tempo máximo excedido", idx + 1, t->plano->passos->len, s->ferramenta);
        enviar_progresso_tarefa(t, "observando", idx, msg, FALSE);
        g_free(msg);
        g_executor->cancelar(g_executor, pc->exec_id);
        tarefa_avancar(t);
    }
    return G_SOURCE_REMOVE;
}

/* Interrompe tudo que está em execução e marca o que não começou. Não
 * envia nada: quem chama decide a mensagem final. */
static void tarefa_parar_passos(Tarefa *t, PassoEstado estado_dos_executando, const char *motivo)
{
    for (guint i = 0; i < t->plano->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(t->plano->passos, i);
        if (t->timer_passo[i]) { g_source_remove(t->timer_passo[i]); t->timer_passo[i] = 0; }
        if (s->estado == PASSO_EXECUTANDO) {
            s->estado = estado_dos_executando; /* antes de cancelar: callback síncrono do executor precisa ver o passo já resolvido */
            s->duracao_ms = (g_get_monotonic_time() - t->inicio_passo[i]) / 1000;
            g_free(s->erro);
            s->erro = g_strdup(motivo);
            if (t->exec_passo[i]) g_executor->cancelar(g_executor, t->exec_passo[i]);
        } else if (s->estado == PASSO_AGUARDANDO) {
            s->estado = PASSO_PULADO;
            g_free(s->erro);
            s->erro = g_strdup_printf("não executado: %s", motivo);
        }
    }
}

static void tarefa_desligar_do_cliente(Tarefa *t)
{
    Cliente *c = t->c;
    t->encerrada = TRUE;
    if (t->timer_global) { g_source_remove(t->timer_global); t->timer_global = 0; }
    if (c->tarefa == t) {
        c->tarefa = NULL;
        c->estado = ESTADO_CONECTADO;
        tarefa_unref(t); /* a referência que o Cliente segurava */
    }
}

static gboolean ao_timeout_global(gpointer dados)
{
    Tarefa *t = dados;
    t->timer_global = 0;
    if (t->encerrada) return G_SOURCE_REMOVE;
    g_print("lia-sessiond: tarefa %s: orçamento de tempo (%d ms) esgotado\n", t->plano->plano_id, t->plano->orcamento_tempo_ms);
    tarefa_ref(t);
    tarefa_parar_passos(t, PASSO_FALHA, "orçamento de tempo da tarefa esgotado");
    char **aus = NULL;
    AgenteVeredito v = agente_verificar(t->plano, FALSE, &aus);
    tarefa_finalizar(t, v, aus, "o orçamento de tempo da tarefa acabou antes de todos os passos terminarem");
    g_strfreev(aus);
    tarefa_unref(t);
    return G_SOURCE_REMOVE;
}

static void tarefa_despachar(Tarefa *t, int idx)
{
    AgentePasso *s = g_ptr_array_index(t->plano->passos, (guint)idx);
    const ExecutorManifesto *mf = g_executor->buscar_manifesto(s->ferramenta);
    s->estado = PASSO_EXECUTANDO;
    g_free(t->exec_passo[idx]);
    t->exec_passo[idx] = lia_novo_id("exec");
    t->inicio_passo[idx] = g_get_monotonic_time();
    char *msg = g_strdup_printf("passo %d/%u %s: iniciado", idx + 1, t->plano->passos->len, s->ferramenta);
    enviar_progresso_tarefa(t, "executando", idx, msg, FALSE);
    g_free(msg);
    t->timer_passo[idx] = g_timeout_add_full(G_PRIORITY_DEFAULT, (guint)s->timeout_ms, ao_timeout_passo,
                                              passo_ctx_novo(t, idx), passo_ctx_liberar);
    JsonParser *jp = json_parser_new();
    JsonObject *params = NULL;
    if (json_parser_load_from_data(jp, s->parametros_json, -1, NULL) && json_parser_get_root(jp) &&
        JSON_NODE_TYPE(json_parser_get_root(jp)) == JSON_NODE_OBJECT)
        params = json_node_get_object(json_parser_get_root(jp));
    /* o executor guarda a própria referência ao objeto de parâmetros */
    g_executor->iniciar(g_executor, t->exec_passo[idx], mf, params, ao_passo_saida, ao_passo_progresso,
                         ao_passo_concluido, passo_ctx_novo(t, idx));
    g_object_unref(jp);
}

/* ---- replanejamento com sugestão do modelo (só quando o determinístico não tem mais o que tentar) ---- */
static void ao_sugestao_replanejamento(gpointer ctx, const ModeloResposta *r)
{
    Tarefa *t = ctx;
    gboolean valida = !t->encerrada && t->modelo_id != NULL;
    g_clear_pointer(&t->modelo_id, g_free);
    if (valida) {
        char *recusa = NULL;
        if (!r->sucesso)
            recusa = g_strdup_printf("o modelo não respondeu (%s)", r->erro_codigo ? r->erro_codigo : "erro");
        else if (!r->ferramenta_proposta)
            recusa = g_strdup("o modelo não sugeriu nenhuma ferramenta");
        else
            recusa = agente_replanejar_com_sugestao(t->plano, r->ferramenta_proposta, r->parametros_json, &AG_MANIFESTO);
        if (!recusa) {
            g_print("lia-sessiond: tarefa %s: replanejamento %d — passo sugerido pelo modelo aceito (%s)\n",
                    t->plano->plano_id, t->plano->replanejamentos, r->ferramenta_proposta);
            enviar_progresso_tarefa(t, "replanejando", -1, "replanejamento: passo de leitura sugerido pelo modelo, validado e acrescentado", TRUE);
            tarefa_avancar(t);
        } else {
            /* sugestão recusada pelo validador: registra como limitação e
             * conclui com o que há — nunca executa o que foi recusado */
            char *lim = g_strdup_printf("replanejamento: sugestão do modelo recusada pelo validador (%s%s%s) — nada foi executado a partir dela",
                                         r->ferramenta_proposta ? r->ferramenta_proposta : "", r->ferramenta_proposta ? ": " : "", recusa);
            g_print("lia-sessiond: tarefa %s: %s\n", t->plano->plano_id, lim);
            guint n = t->plano->limitacoes ? g_strv_length(t->plano->limitacoes) : 0;
            t->plano->limitacoes = g_realloc(t->plano->limitacoes, sizeof(char *) * (n + 2));
            t->plano->limitacoes[n] = lim;
            t->plano->limitacoes[n + 1] = NULL;
            g_free(recusa);
            char **aus = NULL;
            AgenteVeredito v = agente_verificar(t->plano, FALSE, &aus);
            tarefa_finalizar(t, v, aus, NULL);
            g_strfreev(aus);
        }
    }
    tarefa_unref(t);
}

static void tarefa_verificar(Tarefa *t)
{
    t->estado = TAREFA_VERIFICANDO;
    enviar_progresso_tarefa(t, "verificando", -1, "verificando os critérios de sucesso contra as evidências coletadas", FALSE);
    gint64 restante_ms = t->plano->orcamento_tempo_ms - (g_get_monotonic_time() - t->inicio_us) / 1000;
    gboolean pode = t->plano->replanejamentos < AGENTE_MAX_REPLANEJAMENTOS &&
                    (int)t->plano->passos->len < t->plano->orcamento_passos && restante_ms > 2000;
    char **aus = NULL;
    AgenteVeredito v = agente_verificar(t->plano, pode, &aus);
    if (v == VEREDITO_REPLANEJAR) {
        t->estado = TAREFA_REPLANEJANDO;
        int n = agente_replanejar(t->plano, aus, &AG_MANIFESTO);
        if (n > 0) {
            char *lista = g_strjoinv(", ", aus);
            char *msg = g_strdup_printf("replanejamento %d de %d: %d passo(s) de leitura acrescentado(s) para obter: %s",
                                         t->plano->replanejamentos, AGENTE_MAX_REPLANEJAMENTOS, n, lista);
            g_print("lia-sessiond: tarefa %s: %s\n", t->plano->plano_id, msg);
            enviar_progresso_tarefa(t, "replanejando", -1, msg, TRUE);
            g_free(msg);
            g_free(lista);
            g_strfreev(aus);
            tarefa_avancar(t);
            return;
        }
        /* o replanejador determinístico não tem mais o que tentar: o modelo
         * pode SUGERIR um passo — só os nomes das evidências entram no
         * prompt, nunca o conteúdo devolvido pelas ferramentas (dado de
         * ferramenta não vira instrução), e a sugestão passa pelo
         * validador como qualquer outra. Uma vez por tarefa. */
        if (g_modelo && !t->sugestao_pedida && restante_ms > 20000) {
            t->sugestao_pedida = TRUE;
            char *lista = g_strjoinv(", ", aus);
            GString *tentadas = g_string_new(NULL);
            for (guint i = 0; i < t->plano->passos->len; i++) {
                AgentePasso *s = g_ptr_array_index(t->plano->passos, i);
                g_string_append_printf(tentadas, "%s%s (%s)", i ? ", " : "", s->ferramenta, agente_passo_estado_texto(s->estado));
            }
            char *pedido = g_strdup_printf("Replanejamento de uma tarefa de leitura.\nObjetivo: %s\nFerramentas já tentadas: %s.\n"
                                            "Ainda falta obter: %s.\nSugira UMA ferramenta de leitura da lista que possa obter o que falta.",
                                            t->plano->objetivo, tentadas->str, lista);
            char *manifesto_texto = construir_manifesto_texto();
            char *prompt = NULL;
            PromptErro pe = prompt_montar(manifesto_texto, pedido, NULL, &prompt);
            g_free(manifesto_texto);
            g_free(pedido);
            g_free(lista);
            g_string_free(tentadas, TRUE);
            if (pe == PROMPT_OK) {
                char *esquema = construir_esquema_proposta();
                ModeloOpcoes op = { .esquema_json = esquema, .max_tokens = LIA_MODELO_TOKENS_PROPOSTA, .resumo = FALSE };
                g_free(t->modelo_id);
                t->modelo_id = g_strdup_printf("%s-replano", t->plano->plano_id);
                char *id = g_strdup(t->modelo_id);
                enviar_progresso_tarefa(t, "replanejando", -1, "replanejamento: consultando o modelo por um próximo passo de leitura", FALSE);
                g_modelo->perguntar(g_modelo, id, prompt, &op, ao_sugestao_replanejamento, tarefa_ref(t));
                g_free(id);
                g_free(esquema);
                g_free(prompt);
                g_strfreev(aus);
                return;
            }
            g_free(prompt);
        }
        g_strfreev(aus);
        v = agente_verificar(t->plano, FALSE, &aus);
    }
    tarefa_finalizar(t, v, aus, NULL);
    g_strfreev(aus);
}

static void tarefa_avancar(Tarefa *t)
{
    if (t->encerrada) return;
    int idx[AGENTE_MAX_PASSOS], pulados = 0;
    int n = agente_passos_prontos(t->plano, idx, AGENTE_MAX_PASSOS, &pulados);
    if (n > 0) t->estado = TAREFA_EXECUTANDO;
    for (int i = 0; i < n && !t->encerrada; i++)
        tarefa_despachar(t, idx[i]);
    if (t->encerrada) return;
    gboolean executando = FALSE, aguardando = FALSE;
    for (guint i = 0; i < t->plano->passos->len; i++) {
        PassoEstado e = ((AgentePasso *)g_ptr_array_index(t->plano->passos, i))->estado;
        if (e == PASSO_EXECUTANDO) executando = TRUE;
        if (e == PASSO_AGUARDANDO) aguardando = TRUE;
    }
    if (!executando && !aguardando)
        tarefa_verificar(t);
}

/* ---- resposta final: relatório determinístico (+ leitura do modelo, conferida) ---- */
typedef struct {
    Cliente *c;
    char *pergunta_id, *modelo_id, *relatorio_base, *evidencias, *pedido, *plano_json;
    AgenteVeredito v;
    char **ausentes;
    gint64 inicio_us;
} ComentarioCtx;

static void comentario_ctx_liberar(ComentarioCtx *cc)
{
    cliente_unref(cc->c);
    g_free(cc->pergunta_id);
    g_free(cc->modelo_id);
    g_free(cc->relatorio_base);
    g_free(cc->evidencias);
    g_free(cc->pedido);
    g_free(cc->plano_json);
    g_strfreev(cc->ausentes);
    g_free(cc);
}

static void ao_comentario_modelo(gpointer ctx, const ModeloResposta *r)
{
    ComentarioCtx *cc = ctx;
    Cliente *c = cc->c;
    gboolean valido = !c->fechado && c->resumo_pendente_id && strcmp(c->resumo_pendente_id, cc->modelo_id) == 0;
    if (valido) {
        g_clear_pointer(&c->resumo_pendente_id, g_free);
        gint64 tempo_ms = (g_get_monotonic_time() - cc->inicio_us) / 1000;
        const char *evs[] = { cc->relatorio_base, cc->evidencias, cc->pedido, NULL };
        char *suspeito = NULL, *aviso = NULL;
        gboolean aceito = FALSE;
        if (!r->sucesso)
            aviso = g_strdup_printf("o modelo não gerou a leitura complementar (%s); o relatório acima é completo sem ela",
                                     r->erro_codigo ? r->erro_codigo : "erro");
        else if (!r->resposta_texto || !*r->resposta_texto)
            aviso = g_strdup("o modelo devolveu uma leitura vazia; o relatório acima é completo sem ela");
        else if (apresentacao_resumo_e_recusa(r->resposta_texto))
            aviso = g_strdup("a leitura do modelo foi descartada: negava ter os dados que as ferramentas devolveram");
        else if (agente_afirmacao_de_seguranca(r->resposta_texto))
            aviso = g_strdup("a leitura do modelo foi descartada: fazia uma afirmação absoluta de segurança/infecção, que nenhuma ferramenta desta versão sustenta");
        else if (agente_conclusao_positiva_sem_base(r->resposta_texto))
            aviso = g_strdup("a leitura do modelo foi descartada: concluía que está tudo bem, o que a ausência de indícios nas medições não autoriza");
        else if (!apresentacao_resumo_fundamentado(r->resposta_texto, evs, &suspeito))
            aviso = g_strdup_printf("a leitura do modelo foi descartada: citava o número \"%s\", que não está nas evidências coletadas", suspeito ? suspeito : "?");
        else
            aceito = TRUE;
        g_print("lia-sessiond: tarefa (pergunta %s): leitura do modelo em %" G_GINT64_FORMAT " ms — %s\n",
                cc->pergunta_id, tempo_ms, aceito ? "aceita (conferida)" : "descartada");
        char *texto = aceito ? g_strdup_printf("%s\n\nLeitura do modelo (texto livre; números conferidos contra as evidências acima): %s",
                                                cc->relatorio_base, r->resposta_texto)
                             : g_strdup(cc->relatorio_base);
        enviar_resposta_final(c, cc->pergunta_id, texto, aceito ? "deterministica+modelo" : "deterministica", TRUE, aviso, tempo_ms);
        g_free(texto);
        g_free(aviso);
        g_free(suspeito);
    }
    comentario_ctx_liberar(cc);
}

/* ---- resposta final de pesquisas web (sessiond/sintese_web.h) ---- */
#define LIA_MODELO_TOKENS_SINTESE_WEB 520

typedef struct {
    Cliente *c;
    char *pergunta_id, *modelo_id, *pedido;
    GPtrArray *fontes; /* FonteWeb* */
    gint64 inicio_us;
} SinteseCtx;

static void sintese_ctx_liberar(SinteseCtx *sc)
{
    cliente_unref(sc->c);
    g_free(sc->pergunta_id);
    g_free(sc->modelo_id);
    g_free(sc->pedido);
    if (sc->fontes) g_ptr_array_unref(sc->fontes);
    g_free(sc);
}

/* andamento da escrita da resposta: 'progresso' SEM execucao_id (a coleta
 * já terminou; isto é só o modelo escrevendo), com o pergunta_id */
static void ao_sintese_progresso(gpointer ctx, gint64 tokens)
{
    SinteseCtx *sc = ctx;
    Cliente *c = sc->c;
    if (c->fechado || !c->resumo_pendente_id || strcmp(c->resumo_pendente_id, sc->modelo_id) != 0) return;
    gint64 seg = (g_get_monotonic_time() - sc->inicio_us) / G_USEC_PER_SEC;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "pergunta_id");
    json_builder_add_string_value(b, sc->pergunta_id);
    json_builder_set_member_name(b, "fracao");
    json_builder_add_double_value(b, MIN(1.0, (double)tokens / LIA_MODELO_TOKENS_SINTESE_WEB));
    char *msg = g_strdup_printf("organizando a resposta: %" G_GINT64_FORMAT " tokens escritos em %" G_GINT64_FORMAT " s", tokens, seg);
    json_builder_set_member_name(b, "mensagem");
    json_builder_add_string_value(b, msg);
    g_free(msg);
    json_builder_set_member_name(b, "sintese");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tokens");
    json_builder_add_int_value(b, tokens);
    json_builder_set_member_name(b, "max_tokens");
    json_builder_add_int_value(b, LIA_MODELO_TOKENS_SINTESE_WEB);
    json_builder_set_member_name(b, "fontes");
    json_builder_add_int_value(b, sc->fontes->len);
    json_builder_end_object(b);
    json_builder_end_object(b);
    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_PROGRESSO, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void ao_sintese_web(gpointer ctx, const ModeloResposta *r)
{
    SinteseCtx *sc = ctx;
    Cliente *c = sc->c;
    gboolean valido = !c->fechado && c->resumo_pendente_id && strcmp(c->resumo_pendente_id, sc->modelo_id) == 0;
    if (valido) {
        g_clear_pointer(&c->resumo_pendente_id, g_free);
        gint64 tempo_ms = (g_get_monotonic_time() - sc->inicio_us) / 1000;
        char *motivo = NULL;
        int descartados = 0;
        char *texto = r->sucesso ? sintese_web_renderizar(r->resposta_texto, sc->fontes, sc->pedido, &descartados, &motivo) : NULL;
        if (g_strcmp0(g_getenv("LIA_SESSIOND_DEPURAR_SINTESE"), "1") == 0)
            g_print("lia-sessiond: [depuração] saída bruta da síntese: %.3000s\n", r->resposta_texto ? r->resposta_texto : "(nenhuma)");
        const char *origem = "modelo_sintese";
        char *aviso = NULL;
        if (!texto) {
            /* o modelo falhou ou nada da síntese conferiu: resposta extrativa,
             * ainda conclusiva e organizada, só com frases das fontes */
            char *nota = r->sucesso
                ? g_strdup_printf("a síntese do modelo não pôde ser usada (%s)", motivo ? motivo : "inválida")
                : g_strdup_printf("o modelo não respondeu (%s: %s)", r->erro_codigo ? r->erro_codigo : "erro",
                                  r->erro_mensagem ? r->erro_mensagem : "sem detalhe");
            texto = sintese_web_extrativa(sc->fontes, sc->pedido, nota);
            aviso = nota;
            origem = "extrativa";
        } else if (motivo) {
            aviso = g_strdup_printf("parte da síntese do modelo foi descartada por não conferir com as fontes: %s", motivo);
        }
        g_print("lia-sessiond: pergunta %s: resposta web (%s) em %" G_GINT64_FORMAT " ms, %u fonte(s), %d item(ns) descartado(s)%s%s\n",
                sc->pergunta_id, origem, tempo_ms, sc->fontes->len, descartados, motivo ? " — " : "", motivo ? motivo : "");
        enviar_resposta_final(c, sc->pergunta_id, texto, origem, TRUE, aviso, tempo_ms);
        g_free(texto);
        g_free(aviso);
        g_free(motivo);
    }
    sintese_ctx_liberar(sc);
}

/* TRUE se tratou (e enviou, ou agendou) a resposta final desta tarefa web */
static gboolean tarefa_resposta_web(Tarefa *t, Cliente *c, const char *pergunta_id, const char *pedido)
{
    if (strcmp(t->plano->intencao, "pesquisa_web") != 0 && strcmp(t->plano->intencao, "abrir_web") != 0)
        return FALSE;
    GPtrArray *dados = g_ptr_array_new();
    const char *erro = NULL;
    for (guint i = 0; i < t->plano->passos->len; i++) {
        AgentePasso *s = g_ptr_array_index(t->plano->passos, i);
        if (!g_str_has_prefix(s->ferramenta, "web.")) continue;
        if (s->estado == PASSO_SUCESSO && s->dados_json) g_ptr_array_add(dados, s->dados_json);
        else if (s->erro && !erro) erro = s->erro;
    }
    g_ptr_array_add(dados, NULL);
    const char *const *dj = (const char *const *)dados->pdata;
    char *cotacao = sintese_web_cotacao(dj);
    if (cotacao) {
        g_print("lia-sessiond: pergunta %s: resposta web (cotação estruturada, sem modelo)\n", pergunta_id);
        enviar_resposta_final(c, pergunta_id, cotacao, "deterministica", TRUE, NULL, 0);
        g_free(cotacao);
        g_ptr_array_free(dados, TRUE);
        return TRUE;
    }
    GPtrArray *fontes = sintese_web_fontes(dj, pedido, SINTESE_WEB_MAX_FONTES);
    g_ptr_array_free(dados, TRUE);
    if (fontes->len == 0) {
        char *texto = sintese_web_sem_dados(fontes, pedido, erro);
        g_print("lia-sessiond: pergunta %s: resposta web sem fontes (%s)\n", pergunta_id, erro ? erro : "busca sem resultados");
        enviar_resposta_final(c, pergunta_id, texto, "deterministica", TRUE,
                              erro ? "a ferramenta web falhou; o erro real está na resposta" : "a busca não devolveu fontes utilizáveis", 0);
        g_free(texto);
        g_ptr_array_unref(fontes);
        return TRUE;
    }
    if (!g_modelo || g_strcmp0(g_getenv("LIA_SESSIOND_SINTESE_WEB"), "extrativa") == 0) {
        char *texto = sintese_web_extrativa(fontes, pedido, g_modelo ? "síntese pelo modelo desligada (LIA_SESSIOND_SINTESE_WEB=extrativa)" : NULL);
        enviar_resposta_final(c, pergunta_id, texto, "extrativa", TRUE, NULL, 0);
        g_free(texto);
        g_ptr_array_unref(fontes);
        return TRUE;
    }
    SinteseCtx *sc = g_new0(SinteseCtx, 1);
    sc->c = cliente_ref(c);
    sc->pergunta_id = g_strdup(pergunta_id);
    sc->modelo_id = g_strdup_printf("%s-sintese", pergunta_id);
    sc->pedido = g_strdup(pedido);
    sc->fontes = fontes;
    sc->inicio_us = g_get_monotonic_time();
    char *fontes_texto = sintese_web_fontes_para_prompt(fontes);
    GDateTime *agora = g_date_time_new_now_local();
    char *hoje = g_date_time_format(agora, "%Y-%m-%d");
    g_date_time_unref(agora);
    char *prompt = prompt_montar_sintese_web(pedido, fontes_texto, hoje);
    g_free(hoje);
    g_free(fontes_texto);
    g_free(c->resumo_pendente_id);
    c->resumo_pendente_id = g_strdup(sc->modelo_id);
    /* SEM saída restrita por esquema (o formato JSON vai no prompt): com
     * "format", o Ollama só devolve o texto no fim — sem andamento nenhum
     * durante ~1 min. O JSON é lido de forma tolerante (texto em volta,
     * cercas de código, saída cortada): sessiond/sintese_web.c. */
    ModeloOpcoes op = { .esquema_json = NULL, .max_tokens = LIA_MODELO_TOKENS_SINTESE_WEB, .resumo = TRUE,
                        .progresso_cb = ao_sintese_progresso, .progresso_ctx = sc };
    g_print("lia-sessiond: pergunta %s: sintetizando a resposta a partir de %u fonte(s)\n", pergunta_id, fontes->len);
    char *id = g_strdup(sc->modelo_id);
    g_modelo->perguntar(g_modelo, id, prompt, &op, ao_sintese_web, sc);
    g_free(id);
    g_free(prompt);
    return TRUE;
}

static void tarefa_finalizar(Tarefa *t, AgenteVeredito v, char **ausentes, const char *motivo_extra)
{
    if (t->encerrada) return;
    Cliente *c = t->c;
    tarefa_ref(t);
    t->estado = v == VEREDITO_SUCESSO ? TAREFA_CONCLUIDO : TAREFA_FALHA;
    if (motivo_extra) {
        guint n = t->plano->limitacoes ? g_strv_length(t->plano->limitacoes) : 0;
        t->plano->limitacoes = g_realloc(t->plano->limitacoes, sizeof(char *) * (n + 2));
        t->plano->limitacoes[n] = g_strdup(motivo_extra);
        t->plano->limitacoes[n + 1] = NULL;
    }
    gint64 total_ms = (g_get_monotonic_time() - t->inicio_us) / 1000;
    char *relatorio = agente_relatorio(t->plano, v, ausentes, NULL);
    const char *estado = v == VEREDITO_SUCESSO ? "sucesso" : (v == VEREDITO_INCOMPLETO ? "incompleto" : "falha");
    g_print("lia-sessiond: tarefa %s: %s em %" G_GINT64_FORMAT " ms (%u passos, %d replanejamento(s), veredito=%s)\n",
            t->plano->plano_id, estado, total_ms, t->plano->passos->len, t->plano->replanejamentos, agente_veredito_texto(v));

    /* 'resultado' da tarefa inteira: o plano com a observação de cada passo */
    if (!c->fechado) {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "execucao_id");
        json_builder_add_string_value(b, t->exec_id);
        json_builder_set_member_name(b, "estado");
        json_builder_add_string_value(b, estado);
        json_builder_set_member_name(b, "codigo_saida");
        json_builder_add_int_value(b, v == VEREDITO_SUCESSO ? 0 : (v == VEREDITO_INCOMPLETO ? 2 : 1));
        json_builder_set_member_name(b, "dados");
        json_builder_begin_object(b);
        char *pj = agente_plano_para_json(t->plano, TRUE);
        embutir_json_bruto(b, "tarefa", pj);
        g_free(pj);
        json_builder_set_member_name(b, "veredito");
        json_builder_add_string_value(b, agente_veredito_texto(v));
        json_builder_set_member_name(b, "evidencias_ausentes");
        json_builder_begin_array(b);
        for (int i = 0; ausentes && ausentes[i]; i++) json_builder_add_string_value(b, ausentes[i]);
        json_builder_end_array(b);
        json_builder_end_object(b);
        json_builder_set_member_name(b, "tarefa");
        json_builder_add_boolean_value(b, TRUE);
        json_builder_set_member_name(b, "tempo_execucao_ms");
        json_builder_add_int_value(b, total_ms);
        json_builder_end_object(b);
        char *id = lia_novo_id("msg");
        char *linha = lia_msg_montar(LIA_MSG_RESULTADO, id, NULL, b);
        g_free(id);
        g_object_unref(b);
        escrever_linha(c, linha);
    }

    char *pergunta_id = g_strdup(t->pergunta_id ? t->pergunta_id : t->plano->plano_id);
    int n_ok = 0;
    for (guint i = 0; i < t->plano->passos->len; i++)
        if (((AgentePasso *)g_ptr_array_index(t->plano->passos, i))->estado == PASSO_SUCESSO) n_ok++;
    gint64 restante_ms = t->plano->orcamento_tempo_ms - total_ms;
    /* O relatório determinístico É a resposta. A "leitura do modelo" (duas
     * frases relacionando as evidências) é OPCIONAL e desligada por padrão
     * (LIA_SESSIOND_LEITURA_DO_MODELO=sim liga): medido com o modelo local
     * de 3B, ela multiplicava o tempo da tarefa por ~100 (0,2 s de coleta
     * contra 22–42 s de geração) e, mesmo passando na conferência
     * numérica, afirmava coisas que as medições não sustentam ("o sistema
     * está sem problemas", "alta carga de CPU" para um processo com
     * 0,63 %). Ligada, continua conferida — ver ao_comentario_modelo. */
    gboolean quer_modelo = g_modelo && !c->fechado && n_ok >= 2 && v != VEREDITO_FALHA && restante_ms > 15000 &&
                           strcmp(t->plano->intencao, "inventario_de_hardware") != 0 &&
                           g_strcmp0(g_getenv("LIA_SESSIOND_LEITURA_DO_MODELO"), "sim") == 0;
    char *evid = agente_evidencias_texto(t->plano);
    char *pedido = g_strdup(t->pedido ? t->pedido : t->plano->objetivo);
    tarefa_desligar_do_cliente(t);

    /* pesquisa web: a resposta final é a síntese das fontes lidas (ou a
     * cotação, ou o erro real) — nunca o relatório genérico da coleta */
    if (!c->fechado && tarefa_resposta_web(t, c, pergunta_id, pedido)) {
        g_free(pergunta_id);
        g_free(relatorio);
        g_free(evid);
        g_free(pedido);
        tarefa_unref(t);
        return;
    }

    if (quer_modelo) {
        ComentarioCtx *cc = g_new0(ComentarioCtx, 1);
        cc->c = cliente_ref(c);
        cc->pergunta_id = pergunta_id;
        cc->modelo_id = g_strdup_printf("%s-leitura", pergunta_id);
        cc->relatorio_base = relatorio;
        cc->evidencias = evid;
        cc->pedido = pedido;
        cc->inicio_us = g_get_monotonic_time();
        /* para o modelo vai só o relatório determinístico (conclusão +
         * evidências já resumidas), como DADO delimitado — não as tabelas
         * inteiras: menos tokens, menos tempo, mesma base de conferência. */
        char *prompt = prompt_montar_interpretacao(pedido, relatorio);
        g_free(c->resumo_pendente_id);
        c->resumo_pendente_id = g_strdup(cc->modelo_id);
        ModeloOpcoes op = { .esquema_json = NULL, .max_tokens = LIA_MODELO_TOKENS_RESUMO, .resumo = TRUE };
        char *id = g_strdup(cc->modelo_id);
        g_modelo->perguntar(g_modelo, id, prompt, &op, ao_comentario_modelo, cc);
        g_free(id);
        g_free(prompt);
    } else {
        if (!c->fechado)
            enviar_resposta_final(c, pergunta_id, relatorio, "deterministica", TRUE, NULL, 0);
        g_free(pergunta_id);
        g_free(relatorio);
        g_free(evid);
        g_free(pedido);
    }
    tarefa_unref(t);
}

/* Cancelamento GLOBAL: para todos os passos em execução, pula os
 * pendentes, cancela consulta ao modelo, e responde 'resultado' cancelado. */
static void tarefa_cancelar(Tarefa *t, const char *motivo, gboolean avisar)
{
    if (t->encerrada) return;
    Cliente *c = t->c;
    tarefa_ref(t);
    t->estado = TAREFA_CANCELADO;
    tarefa_parar_passos(t, PASSO_CANCELADO, motivo);
    if (t->modelo_id && g_modelo) {
        char *id = t->modelo_id;
        t->modelo_id = NULL;
        g_modelo->cancelar(g_modelo, id);
        g_free(id);
    }
    g_print("lia-sessiond: tarefa %s: cancelada (%s)\n", t->plano->plano_id, motivo);
    if (avisar && !c->fechado) {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "execucao_id");
        json_builder_add_string_value(b, t->exec_id);
        json_builder_set_member_name(b, "estado");
        json_builder_add_string_value(b, "cancelado");
        json_builder_set_member_name(b, "codigo_saida");
        json_builder_add_int_value(b, 130);
        json_builder_set_member_name(b, "dados");
        json_builder_begin_object(b);
        char *pj = agente_plano_para_json(t->plano, TRUE);
        embutir_json_bruto(b, "tarefa", pj);
        g_free(pj);
        json_builder_end_object(b);
        json_builder_set_member_name(b, "tarefa");
        json_builder_add_boolean_value(b, TRUE);
        json_builder_end_object(b);
        char *id = lia_novo_id("msg");
        char *linha = lia_msg_montar(LIA_MSG_RESULTADO, id, NULL, b);
        g_free(id);
        g_object_unref(b);
        escrever_linha(c, linha);
    }
    tarefa_desligar_do_cliente(t);
    tarefa_unref(t);
}

static void tarefa_iniciar(Tarefa *t)
{
    t->estado = TAREFA_EXECUTANDO;
    t->inicio_us = g_get_monotonic_time();
    t->timer_global = g_timeout_add((guint)t->plano->orcamento_tempo_ms, ao_timeout_global, t);
    t->c->estado = ESTADO_EXECUTANDO;
    g_print("lia-sessiond: tarefa %s: iniciada (%u passos, intenção=%s, hash=%.12s…)\n", t->plano->plano_id,
            t->plano->passos->len, t->plano->intencao, t->plano->hash);
    tarefa_ref(t);
    tarefa_avancar(t);
    tarefa_unref(t);
}

/* Cria a tarefa a partir de um plano JÁ VALIDADO (toma posse dele), envia
 * a mensagem 'plano' e começa — ou para em aprovação. */
static void tarefa_criar(Cliente *c, const char *em_resposta_a, AgentePlano *plano, const char *pergunta_id, const char *pedido)
{
    Tarefa *t = g_new0(Tarefa, 1);
    t->refs = 1; /* do Cliente */
    t->c = cliente_ref(c);
    t->plano = plano;
    t->exec_id = lia_novo_id("exec");
    t->pergunta_id = pergunta_id ? g_strdup(pergunta_id) : NULL;
    t->pedido = pedido ? g_strdup(pedido) : NULL;
    t->estado = TAREFA_PLANO_RECEBIDO;
    c->tarefa = t;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    char *pj = agente_plano_para_json(plano, FALSE);
    JsonParser *jp = json_parser_new();
    if (json_parser_load_from_data(jp, pj, -1, NULL)) {
        JsonObject *o = json_node_get_object(json_parser_get_root(jp));
        GList *chaves = json_object_get_members(o);
        for (GList *it = chaves; it; it = it->next) {
            json_builder_set_member_name(b, it->data);
            json_builder_add_value(b, json_node_copy(json_object_get_member(o, it->data)));
        }
        g_list_free(chaves);
    }
    g_object_unref(jp);
    g_free(pj);
    json_builder_set_member_name(b, "tarefa");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "execucao_id");
    json_builder_add_string_value(b, t->exec_id);
    json_builder_set_member_name(b, "web_automatica");
    json_builder_add_boolean_value(b, plano_web_automatico(plano));
    json_builder_set_member_name(b, "ferramenta"); /* compatibilidade com clientes que só leem um nome */
    json_builder_add_string_value(b, "(tarefa de vários passos)");
    json_builder_end_object(b);
    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_PLANO, id, em_resposta_a, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
    if (c->fechado) return;

    if (plano->requer_aprovacao) {
        t->estado = TAREFA_AGUARDANDO_APROVACAO;
        c->estado = ESTADO_AGUARDANDO_APROVACAO;
    } else {
        tarefa_iniciar(t);
    }
}

/* 'proposta' de TAREFA: o plano validado (guardado em c->plano_proposto) ou,
 * quando não há o que executar, só o texto (ferramenta inexistente). */
static void enviar_proposta_tarefa(Cliente *c, const char *pergunta_id, const AgentePlano *plano, AgenteCompat compat,
                                    const char *texto, const char *origem, const char *veredito, const char *motivo,
                                    gint64 tempo_ms, int tentativas)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "pergunta_id");
    json_builder_add_string_value(b, pergunta_id);
    json_builder_set_member_name(b, "tipo_resposta");
    json_builder_add_string_value(b, plano ? "tarefa" : "resposta");
    json_builder_set_member_name(b, "resposta_texto");
    json_builder_add_string_value(b, texto ? texto : "");
    json_builder_set_member_name(b, "ferramenta_proposta");
    json_builder_add_null_value(b);
    json_builder_set_member_name(b, "plano_proposto");
    if (plano) {
        char *pj = agente_plano_para_json(plano, FALSE);
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_data(jp, pj, -1, NULL))
            json_builder_add_value(b, json_node_copy(json_parser_get_root(jp)));
        else
            json_builder_add_null_value(b);
        g_object_unref(jp);
        g_free(pj);
    } else {
        json_builder_add_null_value(b);
    }
    json_builder_set_member_name(b, "intencao");
    if (plano) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "id"); json_builder_add_string_value(b, plano->intencao);
        json_builder_set_member_name(b, "descricao"); json_builder_add_string_value(b, plano->objetivo);
        json_builder_set_member_name(b, "confianca"); json_builder_add_string_value(b, "alta");
        json_builder_end_object(b);
    } else {
        json_builder_add_null_value(b);
    }
    json_builder_set_member_name(b, "validacao");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "origem"); json_builder_add_string_value(b, origem);
    json_builder_set_member_name(b, "veredito"); json_builder_add_string_value(b, veredito);
    json_builder_set_member_name(b, "motivo"); json_builder_add_string_value(b, motivo ? motivo : "");
    /* compativel | parcial | inexistente | esclarecer */
    json_builder_set_member_name(b, "compatibilidade"); json_builder_add_string_value(b, agente_compat_texto(compat));
    json_builder_set_member_name(b, "tentativas"); json_builder_add_int_value(b, tentativas);
    if (plano) {
        json_builder_set_member_name(b, "risco"); json_builder_add_string_value(b, plano->risco);
        json_builder_set_member_name(b, "requer_aprovacao"); json_builder_add_boolean_value(b, plano->requer_aprovacao);
        json_builder_set_member_name(b, "somente_leitura"); json_builder_add_boolean_value(b, plano->somente_leitura);
    }
    json_builder_set_member_name(b, "execucao_automatica_permitida");
    json_builder_add_boolean_value(b, plano && plano->somente_leitura && !plano->requer_aprovacao);
    json_builder_set_member_name(b, "web_automatica");
    json_builder_add_boolean_value(b, plano_web_automatico(plano));
    json_builder_end_object(b);
    json_builder_set_member_name(b, "tempo_modelo_ms");
    json_builder_add_int_value(b, tempo_ms);
    json_builder_end_object(b);
    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_PROPOSTA, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

/* guarda o plano validado para o 'solicitar' seguinte (toma posse) */
static void guardar_plano_proposto(Cliente *c, AgentePlano *plano, const char *pergunta_id, const char *pedido)
{
    agente_plano_liberar(c->plano_proposto);
    c->plano_proposto = plano;
    g_free(c->plano_proposto_pergunta_id);
    c->plano_proposto_pergunta_id = g_strdup(pergunta_id);
    g_free(c->plano_proposto_pedido);
    c->plano_proposto_pedido = g_strdup(pedido);
}

static void tratar_solicitar(Cliente *c, const LiaMsg *m)
{
    if (c->estado != ESTADO_CONECTADO || c->tarefa) {
        enviar_erro(c, m->id, "estado_invalido", "já existe uma solicitação em andamento nesta conexão");
        return;
    }
    const char *ferramenta = campo_string(m->dados, "ferramenta");
    const char *plano_id_pedido = campo_string(m->dados, "plano_id");
    if (!ferramenta && plano_id_pedido) {
        /* Executar um plano de tarefa JÁ validado e proposto pelo daemon.
         * O cliente manda só plano_id + hash: o conteúdo do plano é o que
         * está guardado aqui — nem o cliente nem o modelo conseguem
         * alterá-lo entre a proposta e a execução. Hash diferente (plano
         * de outra versão, ou adulterado) é recusado. */
        const char *hash = campo_string(m->dados, "hash");
        if (!c->plano_proposto || strcmp(c->plano_proposto->plano_id, plano_id_pedido) != 0) {
            enviar_erro(c, m->id, "estado_invalido", "plano_id não corresponde a nenhum plano proposto nesta conexão");
            return;
        }
        if (!hash || strcmp(hash, c->plano_proposto->hash) != 0) {
            enviar_erro(c, m->id, "plano_alterado", "o hash informado não corresponde ao plano validado — nada foi executado");
            return;
        }
        AgentePlano *plano = c->plano_proposto;
        c->plano_proposto = NULL;
        tarefa_criar(c, m->id, plano, c->plano_proposto_pergunta_id, c->plano_proposto_pedido);
        return;
    }
    if (!ferramenta && m->dados && json_object_has_member(m->dados, "passos")) {
        /* Plano de vários passos enviado pelo próprio cliente: mesma
         * validação integral de qualquer plano, antes de qualquer passo. */
        JsonNode *nd = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(nd, m->dados);
        JsonGenerator *g = json_generator_new();
        json_generator_set_root(g, nd);
        char *txt = json_generator_to_data(g, NULL);
        g_object_unref(g);
        json_node_unref(nd);
        char *erro = NULL;
        AgentePlano *plano = agente_plano_de_json(txt, "plano_do_cliente", &erro);
        g_free(txt);
        if (plano && !erro)
            erro = agente_plano_validar(plano, &AG_MANIFESTO);
        if (erro) {
            /* "codigo: mensagem" */
            char *sep = strstr(erro, ": ");
            char *codigo = sep ? g_strndup(erro, (gsize)(sep - erro)) : g_strdup("plano_invalido");
            enviar_erro(c, m->id, codigo, sep ? sep + 2 : erro);
            g_free(codigo);
            g_free(erro);
            agente_plano_liberar(plano);
            return;
        }
        tarefa_criar(c, m->id, plano, NULL, NULL);
        return;
    }
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
            if (strcmp(cap, "leitura_arquivo") != 0 && strcmp(cap, "processo") != 0 && strcmp(cap, "rede") != 0) {
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
    {
        JsonGenerator *gh = json_generator_new();
        json_generator_set_root(gh, c->parametros_pendentes);
        char *params_txt = json_generator_to_data(gh, NULL);
        g_object_unref(gh);
        char *base = g_strdup_printf("%s|%s|%s", c->plano_id_pendente, ferramenta, params_txt ? params_txt : "{}");
        g_free(c->plano_hash_pendente);
        c->plano_hash_pendente = g_compute_checksum_for_string(G_CHECKSUM_SHA256, base, -1);
        g_free(base);
        g_free(params_txt);
    }

    c->estado = ESTADO_PLANO_RECEBIDO;
    enviar_plano(c, m->id, mf);
    if (c->fechado) return;

    if (requer_aprovacao_efetiva(mf)) {
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
    const char *hash_aprovado = campo_string(m->dados, "hash");
    if (c->tarefa) {
        /* plano de tarefa aguardando aprovação: a decisão vale para o
         * plano_id E para o hash do plano validado */
        Tarefa *t = c->tarefa;
        if (!plano_id || strcmp(plano_id, t->plano->plano_id) != 0) {
            enviar_erro(c, m->id, "estado_invalido", "plano_id não corresponde ao plano pendente");
            return;
        }
        if (!hash_aprovado || strcmp(hash_aprovado, t->plano->hash) != 0) {
            enviar_erro(c, m->id, "estado_invalido", "hash não corresponde ao plano validado — a aprovação vale para um plano específico, não só para o plano_id");
            return;
        }
        const char *dec = campo_string(m->dados, "decisao");
        if (!dec || (strcmp(dec, "aprovar") != 0 && strcmp(dec, "rejeitar") != 0)) {
            enviar_erro(c, m->id, "parametro_invalido", "'decisao' deve ser 'aprovar' ou 'rejeitar'");
            return;
        }
        if (strcmp(dec, "aprovar") == 0) tarefa_iniciar(t);
        else tarefa_cancelar(t, "rejeitado pela pessoa", TRUE);
        return;
    }
    if (!plano_id || !c->plano_id_pendente || strcmp(plano_id, c->plano_id_pendente) != 0) {
        /* cobre aprovação de um plano diferente do apresentado. */
        enviar_erro(c, m->id, "estado_invalido", "plano_id não corresponde ao plano pendente");
        return;
    }
    if (!hash_aprovado || !c->plano_hash_pendente || strcmp(hash_aprovado, c->plano_hash_pendente) != 0) {
        /* a aprovação é vinculada ao hash do plano apresentado (plano_id +
         * ferramenta + parâmetros), não só ao plano_id */
        enviar_erro(c, m->id, "estado_invalido", "hash ausente ou diferente do plano apresentado — a aprovação precisa citar o hash do plano");
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

    /* Cancelar a conversa com o modelo (docs/protocolo.md §10): pedido
     * explícito ({"pergunta": true}) ou, sem nenhum plano/execução em
     * andamento, um 'cancelar' simples enquanto o modelo ainda trabalha.
     * A pendência é limpa ANTES de pedir o cancelamento ao backend: o
     * callback dele pode vir de forma síncrona, e precisa já encontrar a
     * pergunta como "não mais válida" para não enviar nada em duplicidade. */
    gboolean quer_pergunta = m->dados && json_object_has_member(m->dados, "pergunta") &&
        JSON_NODE_TYPE(json_object_get_member(m->dados, "pergunta")) == JSON_NODE_VALUE &&
        json_node_get_value_type(json_object_get_member(m->dados, "pergunta")) == G_TYPE_BOOLEAN &&
        json_object_get_boolean_member(m->dados, "pergunta");
    if (c->tarefa && !quer_pergunta) {
        /* cancelamento GLOBAL da tarefa: com o execucao_id dela, ou sem id nenhum */
        if (exec_id_pedido && *exec_id_pedido && strcmp(exec_id_pedido, c->tarefa->exec_id) != 0) {
            enviar_erro(c, m->id, "estado_invalido", "execucao_id não corresponde à tarefa em andamento");
            return;
        }
        tarefa_cancelar(c->tarefa, "cancelado pelo cliente", TRUE);
        return;
    }
    if (quer_pergunta || (c->estado == ESTADO_CONECTADO && (c->pergunta_pendente_id || c->resumo_pendente_id))) {
        if (c->pergunta_pendente_id) {
            char *modelo_id = c->pergunta_modelo_id;
            c->pergunta_modelo_id = NULL;
            g_free(c->pergunta_pendente_id);
            c->pergunta_pendente_id = NULL;
            enviar_erro(c, m->id, "cancelado", "pergunta ao modelo cancelada a pedido do cliente");
            g_print("lia-sessiond: pergunta ao modelo cancelada pelo cliente\n");
            if (modelo_id) g_modelo->cancelar(g_modelo, modelo_id);
            g_free(modelo_id);
        } else if (c->resumo_pendente_id) {
            char *modelo_id = c->resumo_pendente_id;
            c->resumo_pendente_id = NULL;
            enviar_erro(c, m->id, "cancelado", "resumo final cancelado a pedido do cliente (os dados já entregues continuam válidos)");
            g_print("lia-sessiond: resumo final cancelado pelo cliente\n");
            g_modelo->cancelar(g_modelo, modelo_id);
            g_free(modelo_id);
        } else {
            enviar_erro(c, m->id, "estado_invalido", "nenhuma pergunta ao modelo em andamento para cancelar");
        }
        return;
    }

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
    /* ninguém mais vai ler a resposta: não deixa o modelo gerando à toa
     * (encerrar o cliente no meio do streaming). c->fechado já é TRUE, então
     * qualquer callback que venha daqui não envia nada. */
    if (g_modelo && c->pergunta_modelo_id) {
        char *id = c->pergunta_modelo_id;
        c->pergunta_modelo_id = NULL;
        g_modelo->cancelar(g_modelo, id);
        g_free(id);
    }
    if (g_modelo && c->resumo_pendente_id) {
        char *id = c->resumo_pendente_id;
        c->resumo_pendente_id = NULL;
        g_modelo->cancelar(g_modelo, id);
        g_free(id);
    }
    if (c->tarefa)
        tarefa_cancelar(c->tarefa, "cliente desconectou", FALSE);
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
        g_print("lia-sessiond: executor = real (oito ferramentas locais + web.pesquisar + duas de persistência explícita, docs/manifesto.md)\n");
    }
}

/* Diferente do executor: "ollama"/"llama_cpp" exigem um servidor de
 * verdade rodando; "mock" não exige nada. Por isso, ao contrário do
 * executor (onde o padrão é "real", sem dependência extra), o padrão
 * aqui é "mock" — o modelo real é opt-in explícito
 * (LIA_SESSIOND_MODELO=ollama ou LIA_SESSIOND_MODELO=llama_cpp), nunca
 * uma dependência silenciosa de produção. */
static void escolher_modelo(void)
{
    const char *modo = g_getenv("LIA_SESSIOND_MODELO");
    if (modo && strcmp(modo, "ollama") == 0) {
        g_modelo = modelo_ollama_obter();
        g_print("lia-sessiond: modelo = ollama (LIA_SESSIOND_MODELO=ollama)\n");
    } else if (modo && strcmp(modo, "llama_cpp") == 0) {
        g_modelo = modelo_llama_cpp_obter();
        g_print("lia-sessiond: modelo = llama_cpp (LIA_SESSIOND_MODELO=llama_cpp)\n");
    } else {
        g_modelo = modelo_mock_obter();
        g_print("lia-sessiond: modelo = mock (padrão — defina LIA_SESSIOND_MODELO=ollama ou =llama_cpp para o modelo real)\n");
    }
}

static void ao_aquecimento(gpointer ctx, const ModeloResposta *r)
{
    gint64 *inicio = ctx;
    g_print("lia-sessiond: aquecimento do modelo %s em %" G_GINT64_FORMAT " ms\n",
            r->sucesso ? "concluído" : "falhou", (g_get_monotonic_time() - *inicio) / 1000);
    g_free(inicio);
}

/* LIA_SESSIOND_AQUECER_MODELO=sim (chat/rodar_local.sh liga): carrega o
 * modelo real na memória logo que o daemon sobe, em segundo plano, para a
 * primeira pergunta não pagar o carregamento. Desligado por padrão (os
 * testes com servidor falso não devem receber requisições extras). */
static void aquecer_modelo(void)
{
    if (!g_modelo || !strcmp(g_modelo->nome_backend, "mock") ||
        g_strcmp0(g_getenv("LIA_SESSIOND_AQUECER_MODELO"), "sim") != 0)
        return;
    gint64 *inicio = g_new(gint64, 1);
    *inicio = g_get_monotonic_time();
    ModeloOpcoes op = { .esquema_json = NULL, .max_tokens = 1, .resumo = TRUE };
    g_print("lia-sessiond: aquecendo o modelo (%s) em segundo plano\n", g_modelo->nome_backend);
    g_modelo->perguntar(g_modelo, "aquecimento", "Responda apenas: ok", &op, ao_aquecimento, inicio);
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
    /* glibc reserva uma arena de malloc de 64 MiB (virtual) por thread que
     * aloca memória; com RLIMIT_AS abaixo, as threads de abertura de fontes
     * da pesquisa web (web/pesquisa.c) esgotavam o espaço de endereçamento.
     * Duas arenas bastam para este daemon (laço principal + executor). */
    mallopt(M_ARENA_MAX, 2);
    if (!g_getenv("LIA_SESSIOND_SEM_RLIMIT_PARA_TESTE")) {
        struct rlimit rl = { .rlim_cur = (rlim_t)LIA_SESSIOND_RLIMIT_AS_MB * 1024 * 1024,
                              .rlim_max = (rlim_t)LIA_SESSIOND_RLIMIT_AS_MB * 1024 * 1024 };
        if (setrlimit(RLIMIT_AS, &rl) != 0)
            g_printerr("lia-sessiond: aviso: não foi possível aplicar RLIMIT_AS: %s\n", g_strerror(errno));
    }

    escolher_executor();
    escolher_modelo();
    aquecer_modelo();
    g_web_automatica = g_strcmp0(g_getenv("LIA_SESSIOND_WEB_AUTOMATICA"), "sim") == 0;
    g_print("lia-sessiond: web automática: %s\n", g_web_automatica
            ? "LIGADA (web.pesquisar/web.abrir sem confirmação; demais ações continuam exigindo aprovação)"
            : "desligada (toda pesquisa web pede aprovação)");
    const char *rot = g_getenv("LIA_SESSIOND_ROTEAMENTO");
    if (!rot || !*rot || strcmp(rot, "direto") == 0) {
        g_rota_direta = TRUE;
    } else if (strcmp(rot, "modelo") == 0) {
        g_rota_direta = FALSE;
    } else {
        g_printerr("lia-sessiond: LIA_SESSIOND_ROTEAMENTO='%s' desconhecido — use 'direto' ou 'modelo'\n", rot);
        return 1;
    }
    g_print("lia-sessiond: roteamento = %s\n", g_rota_direta
        ? "direto (intenção de alta confiança escolhe a ferramenta sem consultar o modelo; LIA_SESSIOND_ROTEAMENTO=modelo desliga)"
        : "modelo (o modelo sempre propõe; o roteador valida e corrige)");

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
