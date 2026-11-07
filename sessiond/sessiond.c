/* lia-sessiond — daemon de sessão local (servidor de protocolo + máquina
 * de estados), ver docs/protocolo.md.
 *
 * NESTA ETAPA: só o protocolo e um executor simulado (sessiond/manifesto_mock.c).
 * Nunca chama shell, system, popen, fork, execve, posix_spawn, sudo ou
 * pkexec; nunca acessa hardware; nunca pede privilégio; nunca interpreta
 * texto de 'dados' como instrução de sistema — só como dado estruturado do
 * protocolo (docs/THREAT-MODEL.md, "dado, nunca instrução"). Toda ação
 * "executada" é inteiramente fictícia (sessiond/manifesto_mock.c).
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../gui/protocolo.h"
#include "manifesto_mock.h"

#define LIA_SESSIOND_LINHA_MAX 65536
#define LIA_SESSIOND_CHUNK 4096

typedef enum {
    ESTADO_CONECTADO = 0,
    ESTADO_PLANO_RECEBIDO,
    ESTADO_AGUARDANDO_APROVACAO,
    ESTADO_EXECUTANDO_SIMULACAO,
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

    GHashTable *ids_vistos; /* ids de mensagens recebidas nesta conexão */
    EstadoSessao estado;
    char *plano_id_pendente;
    char *ferramenta_pendente;
    char *execucao_id_atual;
    JsonNode *parametros_pendentes;
    guint timer_progresso;
    int tick_execucao;
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

/* -------------------------------------------------------- envio: erro --- */

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

static void enviar_plano(Cliente *c, const char *em_resposta_a, const ManifestoMock *mf)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "plano_id");
    json_builder_add_string_value(b, c->plano_id_pendente);
    json_builder_set_member_name(b, "ferramenta");
    json_builder_add_string_value(b, mf->nome);
    json_builder_set_member_name(b, "parametros_normalizados");
    json_builder_add_value(b, json_node_copy(c->parametros_pendentes));
    json_builder_set_member_name(b, "capacidades");
    json_builder_begin_array(b);
    for (int i = 0; mf->capacidades[i]; i++)
        json_builder_add_string_value(b, mf->capacidades[i]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "risco");
    json_builder_add_string_value(b, mf->risco);
    json_builder_set_member_name(b, "efeitos");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, mf->efeito_simulado);
    json_builder_end_array(b);
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

static void enviar_saida(Cliente *c, const ManifestoMock *mf)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "execucao_id");
    json_builder_add_string_value(b, c->execucao_id_atual);
    json_builder_set_member_name(b, "fluxo");
    json_builder_add_string_value(b, "stdout");
    json_builder_set_member_name(b, "trecho");
    char *trecho = g_strdup_printf("[mock] %s\n", mf->efeito_simulado);
    json_builder_add_string_value(b, trecho);
    g_free(trecho);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_SAIDA, id, NULL, b);
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

static void enviar_resultado(Cliente *c, const char *estado, int codigo_saida)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "execucao_id");
    json_builder_add_string_value(b, c->execucao_id_atual);
    json_builder_set_member_name(b, "estado");
    json_builder_add_string_value(b, estado);
    json_builder_set_member_name(b, "codigo_saida");
    json_builder_add_int_value(b, codigo_saida);
    json_builder_set_member_name(b, "dados");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "mock");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "ferramenta");
    json_builder_add_string_value(b, c->ferramenta_pendente ? c->ferramenta_pendente : "");
    json_builder_end_object(b);
    json_builder_end_object(b);

    char *id = lia_novo_id("msg");
    char *linha = lia_msg_montar(LIA_MSG_RESULTADO, id, NULL, b);
    g_free(id);
    g_object_unref(b);
    escrever_linha(c, linha);
}

static void enviar_auditoria(Cliente *c, const char *resultado_resumo)
{
    const ManifestoMock *mf = manifesto_mock_buscar(c->ferramenta_pendente);
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
 * append-only, SEM dados sensíveis (nunca parametros/conteúdo — só
 * identidade, ferramenta, risco e o resumo do resultado). Mesma convenção
 * de diretório que cli/privilegio.c já usa (THREAT-MODEL.md §7); formato
 * de linha é o do sessiond, mas, no mesmo arquivo. */
static void registrar_auditoria_disco(Daemon *d, Cliente *c, const char *resultado_resumo)
{
    const ManifestoMock *mf = manifesto_mock_buscar(c->ferramenta_pendente);
    if (g_mkdir_with_parents(d->caminho_dir_auditoria, 0700) != 0 && errno != EEXIST)
        return;

    GDateTime *agora = g_date_time_new_now_utc();
    char *data = g_date_time_format(agora, "%Y-%m-%d");
    char *caminho = g_strdup_printf("%s/%s.log", d->caminho_dir_auditoria, data);
    char *linha = g_strdup_printf(
        "%s uid=%u acao=sessiond.%s risco=%s resultado=%s\n",
        g_date_time_format(agora, "%Y-%m-%d %H:%M:%S UTC"),
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
    g_free(caminho);
    g_free(data);
    g_date_time_unref(agora);
}

/* --------------------------------------------------------- estado/fluxo */

static Daemon *g_daemon_para_auditoria; /* único daemon neste processo */

static void resetar_para_conectado(Cliente *c)
{
    c->estado = ESTADO_CONECTADO;
    g_free(c->plano_id_pendente); c->plano_id_pendente = NULL;
    g_free(c->ferramenta_pendente); c->ferramenta_pendente = NULL;
    g_free(c->execucao_id_atual); c->execucao_id_atual = NULL;
    if (c->parametros_pendentes) { json_node_unref(c->parametros_pendentes); c->parametros_pendentes = NULL; }
    c->tick_execucao = 0;
}

static void finalizar_sucesso(Cliente *c)
{
    enviar_resultado(c, "sucesso", 0);
    if (c->fechado) return;
    enviar_auditoria(c, "sucesso (simulado)");
    if (c->fechado) return;
    registrar_auditoria_disco(g_daemon_para_auditoria, c, "sucesso");
    resetar_para_conectado(c);
}

static void finalizar_cancelado(Cliente *c)
{
    if (c->timer_progresso) { g_source_remove(c->timer_progresso); c->timer_progresso = 0; }
    enviar_resultado(c, "cancelado", 130);
    if (c->fechado) return;
    enviar_auditoria(c, "cancelado");
    if (c->fechado) return;
    registrar_auditoria_disco(g_daemon_para_auditoria, c, "cancelado");
    resetar_para_conectado(c);
}

static gboolean ao_tick_execucao(gpointer dados)
{
    Cliente *c = dados;
    c->tick_execucao++;
    if (c->tick_execucao == 1) {
        enviar_progresso(c, 0.5, "simulando execução (mock)");
        return c->fechado ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
    }
    c->timer_progresso = 0;
    finalizar_sucesso(c);
    return G_SOURCE_REMOVE;
}

static void iniciar_execucao_mock(Cliente *c, const ManifestoMock *mf)
{
    g_free(c->execucao_id_atual);
    c->execucao_id_atual = lia_novo_id("exec");
    c->tick_execucao = 0;
    enviar_saida(c, mf);
    if (c->fechado) return;
    c->timer_progresso = g_timeout_add(250, ao_tick_execucao, c);
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
    const ManifestoMock *mf = manifesto_mock_buscar(ferramenta);
    if (!mf) {
        enviar_erro(c, m->id, "ferramenta_desconhecida", ferramenta);
        return;
    }

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
        c->estado = ESTADO_EXECUTANDO_SIMULACAO;
        iniciar_execucao_mock(c, mf);
    }
}

static void tratar_aprovar(Cliente *c, const LiaMsg *m)
{
    if (c->estado != ESTADO_AGUARDANDO_APROVACAO) {
        enviar_erro(c, m->id, "estado_invalido", "não há plano aguardando aprovação nesta conexão");
        return;
    }
    const char *plano_id = campo_string(m->dados, "plano_id");
    if (!plano_id || !c->plano_id_pendente || strcmp(plano_id, c->plano_id_pendente) != 0) {
        enviar_erro(c, m->id, "estado_invalido", "plano_id não corresponde ao plano pendente");
        return;
    }
    const char *decisao = campo_string(m->dados, "decisao");
    if (!decisao || (strcmp(decisao, "aprovar") != 0 && strcmp(decisao, "rejeitar") != 0)) {
        enviar_erro(c, m->id, "parametro_invalido", "'decisao' deve ser 'aprovar' ou 'rejeitar'");
        return;
    }

    if (strcmp(decisao, "aprovar") == 0) {
        const ManifestoMock *mf = manifesto_mock_buscar(c->ferramenta_pendente);
        c->estado = ESTADO_EXECUTANDO_SIMULACAO;
        iniciar_execucao_mock(c, mf);
    } else {
        finalizar_cancelado(c);
    }
}

static void tratar_cancelar(Cliente *c, const LiaMsg *m)
{
    if (c->estado == ESTADO_AGUARDANDO_APROVACAO || c->estado == ESTADO_EXECUTANDO_SIMULACAO) {
        finalizar_cancelado(c);
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
    default:
        enviar_erro(c, m.id, "estado_invalido", "tipo de mensagem não é permitido vindo do cliente");
        break;
    }
    lia_msg_liberar(&m);
}

/* ------------------------------------------------------------ conexão --- */

/* Libera a memória de um Cliente já marcado 'fechado'. Roda num g_idle,
 * nunca na mesma chamada que decidiu desconectar: várias funções (ex.
 * ao_ler_chunk) continuam lendo c->fechado depois de uma função que elas
 * mesmas chamaram poder ter desconectado o cliente (ex. processar_linha ->
 * desconectar_cliente); se a struct fosse liberada ali mesmo, essa leitura
 * seria um use-after-free. Encontrado com AddressSanitizer — ver
 * docs/THREAT-MODEL.md / relatório desta etapa. */
static gboolean liberar_cliente_idle(gpointer dados)
{
    Cliente *c = dados;
    g_hash_table_destroy(c->ids_vistos);
    g_string_free(c->buffer, TRUE);
    g_free(c->plano_id_pendente);
    g_free(c->ferramenta_pendente);
    g_free(c->execucao_id_atual);
    if (c->parametros_pendentes)
        json_node_unref(c->parametros_pendentes);
    g_object_unref(c->conexao);
    g_free(c);
    return G_SOURCE_REMOVE;
}

static void desconectar_cliente(Cliente *c, const char *motivo)
{
    if (c->fechado)
        return;
    c->fechado = TRUE;
    g_print("lia-sessiond: cliente desconectado (%s)\n", motivo ? motivo : "?");
    if (c->timer_progresso) {
        g_source_remove(c->timer_progresso);
        c->timer_progresso = 0;
    }
    g_io_stream_close(G_IO_STREAM(c->conexao), NULL, NULL);
    c->daemon->clientes = g_list_remove(c->daemon->clientes, c);
    g_idle_add(liberar_cliente_idle, c);
}

static void ao_ler_chunk(GObject *origem, GAsyncResult *res, gpointer dados)
{
    Cliente *c = dados;
    GError *erro = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(origem), res, &erro);
    if (n < 0) {
        if (erro) g_error_free(erro);
        desconectar_cliente(c, "erro de leitura");
        return;
    }
    if (n == 0) {
        desconectar_cliente(c, "eof (cliente encerrou)");
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
            return;
        }
        char *linha = g_strndup(c->buffer->str, tam);
        g_string_erase(c->buffer, 0, (gssize)(tam + 1));
        processar_linha(c, linha);
        g_free(linha);
        if (c->fechado)
            return;
    }

    if (c->buffer->len > LIA_SESSIOND_LINHA_MAX) {
        enviar_erro(c, NULL, "limite_excedido", "linha acima do limite de tamanho (sem quebra de linha)");
        if (!c->fechado)
            desconectar_cliente(c, "linha longa sem terminador");
        return;
    }
    comecar_leitura(c);
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
        if (c->timer_progresso) g_source_remove(c->timer_progresso);
        g_io_stream_close(G_IO_STREAM(c->conexao), NULL, NULL);
    }
    g_main_loop_quit(d->loop);
    return G_SOURCE_REMOVE;
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
    /* defesa em profundidade: novos arquivos/dirs nascem restritos ao
     * próprio usuário (sem acesso de grupo/outros); 0077, não 0177 — um
     * umask que também removesse o bit de execução do dono quebraria a
     * criação de diretórios intermediários (mkdir -p precisa conseguir
     * entrar em cada nível para criar o próximo). */
    umask(0077);

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
