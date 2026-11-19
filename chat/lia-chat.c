/* lia-chat — primeiro cliente de chat interativo de terminal para o
 * protocolo de sessão (docs/protocolo.md §9, docs/THREAT-MODEL.md §5.2).
 *
 * Camada fina sobre gui/protocolo.c (não duplica a máquina de estados do
 * protocolo nem a lógica do daemon): só adiciona um REPL de terminal, um
 * fluxo de aprovação explícita e anexação de arquivos. Reaproveita
 * diretamente sessiond/modelo_mock.c e sessiond/modelo_ollama.c (via
 * ModeloBackend) só para o "/modelo"/"/status" fazerem o MESMO probe de
 * disponibilidade que o daemon faria — nunca para aprovar/executar nada
 * (lia-chat não tem executor, não tem acesso a nenhuma ferramenta além do
 * socket do protocolo).
 *
 * Diferente da GUI (gui/lia-gui.c, somente leitura por desenho), este
 * cliente PODE enviar 'aprovar' — é o próprio motivo de existir — então
 * desliga sessao.modo_leitura explicitamente logo após lia_sessao_iniciar.
 *
 * Nunca chama system/popen/fork/execve/posix_spawn/sudo/pkexec. Nunca
 * interpreta texto do usuário como shell. Nunca aprova nada por conta
 * própria (padrão é sempre rejeitar — ver tratar_decisao_plano). Nunca
 * altera um plano recebido: envia de volta exatamente o plano_id que
 * recebeu, sem reconstruir nada.
 *
 * Fluxo agente (docs/protocolo.md §10): uma pergunta segue, sem parar,
 *   pergunta -> proposta validada pelo daemon -> execução -> dados reais
 *            -> resposta final
 * quando — e só quando — o DAEMON marca a proposta como leitura pura
 * (validacao.execucao_automatica_permitida). O que este cliente faz nesse
 * caso é enviar o mesmo 'solicitar' que /plano enviaria; quem valida,
 * planeja e executa continua sendo o daemon (lia-chat não tem executor).
 * Política: LIA_CHAT_AUTOPILOT=read-only (padrão) ou =manual (toda
 * ferramenta exige /plano); qualquer outro valor recusa iniciar. Nada que
 * grave, administre ou toque hardware roda sem aprovação explícita, com
 * qualquer valor dessa variável.
 *
 * Terminal interativo: chat/terminal.c é o dono único do desenho —
 * histórico acima, linha de status (fase atual, indicador de atividade,
 * tempo decorrido) e uma linha de entrada única embaixo. Editor de linha
 * próprio (UTF-8, setas, histórico só em memória, colagem); nenhuma
 * biblioteca de readline. Histórico nunca é gravado em disco (uma linha
 * de chat pode conter qualquer coisa que a pessoa tenha digitado).
 * A linha de status mostra apenas estados operacionais observáveis
 * (consultando modelo, executando leitura...) — nunca texto de raciocínio
 * do modelo.
 */
#define _DEFAULT_SOURCE
#include "../gui/protocolo.h"
#include "../sessiond/modelo.h"
#include "../sessiond/prompt.h"
#include "terminal.h"
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <locale.h>

/* ------------------------------------------------------------- tipos --- */

typedef struct {
    char *caminho;   /* como o usuário digitou — rótulo, nunca confiável */
    char *conteudo;  /* lido uma vez, no momento de /arquivo adicionar */
    gsize tamanho;
} ArquivoAnexo;

static void arquivo_anexo_liberar(ArquivoAnexo *a)
{
    if (!a) return;
    g_free(a->caminho);
    g_free(a->conteudo);
    g_free(a);
}

typedef struct {
    char *pergunta_id;
    char *ferramenta;
    char *parametros_json; /* já serializado — reenviado como está em 'solicitar' */
    char *justificativa;
    /* proposta de TAREFA (plano de vários passos já validado pelo daemon):
     * o 'solicitar' leva só plano_id + hash — nunca o conteúdo do plano */
    char *plano_id;
    char *plano_hash;
} PropostaPendente;

static void proposta_pendente_liberar(PropostaPendente *p)
{
    if (!p) return;
    g_free(p->pergunta_id);
    g_free(p->ferramenta);
    g_free(p->parametros_json);
    g_free(p->justificativa);
    g_free(p->plano_id);
    g_free(p->plano_hash);
    g_free(p);
}

typedef struct {
    LiaSessao sessao;
    GSocketConnection *conexao;
    GOutputStream *saida;
    GDataInputStream *entrada_socket;
    GDataInputStream *entrada_stdin; /* só usado quando !interativo (ver main()) */
    Term *term;             /* != NULL só no modo interativo humano (tty e sem --json): chat/terminal.c */
    GString *buffer_saida;  /* saida_linha acumula; flush_saida escreve tudo de uma vez acima da região viva */
    GString *historico_conversa; /* memória curta, só em RAM; enviada como contexto para a próxima pergunta */
    GMainLoop *loop;

    char *socket_caminho;
    char *backend_assumido;  /* "mock", "ollama" ou "llama_cpp" — só exibição/probe local, nunca controla o daemon remotamente */
    char *modelo_assumido;   /* só repassado para LIA_OLLAMA_MODELO antes do probe */
    gboolean modo_json;      /* --json: NDJSON cru em vez do REPL humano */
    gboolean interativo;     /* isatty(stdin) */

    /* O protocolo não dá ao cliente nenhum id de pergunta ao enviar
     * 'perguntar' (docs/protocolo.md §9) — o "pergunta_id" só existe a
     * partir da resposta ('proposta'/'erro'), atribuído pelo servidor.
     * Como só pode haver uma pergunta em andamento por conexão (o próprio
     * daemon recusa uma segunda — sessiond.c, tratar_perguntar), basta um
     * booleano aqui; o id de verdade só importa quando chega, e nesse
     * momento vai direto para PropostaPendente.pergunta_id. */
    gboolean pergunta_em_andamento;
    gboolean pergunta_abandonada_localmente; /* /cancelar numa pergunta: resposta, quando chegar, é ignorada */
    guint timer_progresso_pergunta; /* 0 = parado; ver iniciar/parar_progresso_pergunta */
    gint64 pergunta_enviada_us;     /* g_get_monotonic_time() de quando a pergunta foi enviada */
    PropostaPendente *proposta_pendente;

    char *plano_id_pendente;
    char *ferramenta_plano_pendente;
    JsonObject *plano_dados_pendente; /* referência; vive enquanto plano_id_pendente != NULL */
    gboolean aguardando_decisao_plano;

    char *execucao_id_pendente;

    char *ultimo_id_enviado;

    GPtrArray *arquivos; /* ArquivoAnexo* */

    gint64 ultimo_sigint_us;

    int codigo_saida;
    gboolean deve_sair;

    /* ---- fluxo agente ---- */
    gboolean autopilot;            /* LIA_CHAT_AUTOPILOT: TRUE = read-only (padrão), FALSE = manual */
    int fase;                      /* Fase: o que a linha de status mostra */
    gboolean turno_espera_resposta;/* a execução atual veio de uma proposta: um 'resposta' final ainda vai chegar */
    gboolean turno_web;            /* pesquisa já exibida; oculta relatório técnico redundante */
    gboolean ocultar_status_pronto; /* após pesquisa web, deixa a região de status vazia */
    char *ferramenta_turno;        /* ferramenta do turno atual (para a linha de status) */
    gint64 turno_inicio_us;        /* pergunta enviada */
    gint64 t_proposta_ms, t_execucao_ms, t_resposta_ms; /* tempos informados pelo daemon; -1 = etapa não ocorreu */
    char *ultimo_dados_json;       /* JSON bruto do último resultado (/dados) */
    char *plano_hash_pendente;     /* hash do plano apresentado: vai junto no 'aprovar' */

    /* ---- tarefa de vários passos em andamento (Agent Runtime v2) ---- */
    gboolean tarefa_ativa;
    char *tarefa_objetivo;
    int tarefa_n;                  /* passos no plano (cresce em replanejamento) */
    char *tarefa_ferramenta[16];
    char *tarefa_estado[16];       /* "aguardando", "executando", "sucesso", "falha"... */
    gint64 tarefa_ms[16];
    gboolean tarefa_replano[16];
    char *tarefa_fase;             /* última fase informada pelo daemon */
} LiaChat;

#define TAREFA_MAX_PASSOS_UI 16

typedef enum {
    FASE_OCIOSO = 0,
    FASE_CONSULTANDO_MODELO,
    FASE_VALIDANDO_PLANO,
    FASE_AGUARDANDO_APROVACAO,
    FASE_EXECUTANDO,
    FASE_GERANDO_RESPOSTA,
} Fase;

/* --------------------------------------------------------- utilidades --- */

/* saida_linha só ACUMULA; flush_saida(), chamada uma vez por "turno"
 * (depois de uma linha processada, ou de uma mensagem do daemon), escreve
 * tudo de uma vez: no terminal interativo, acima da região viva
 * (term_imprimir — nunca sobre a linha de entrada); fora dele, em stdout. */
static void saida_linha(LiaChat *c, const char *fmt, ...)
{
    if (c->modo_json) return; /* --json: só NDJSON cru vai para stdout */
    va_list ap;
    va_start(ap, fmt);
    char *texto = NULL;
    int n = vasprintf(&texto, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    g_string_append(c->buffer_saida, texto);
    g_string_append_c(c->buffer_saida, '\n');
    free(texto);
}

static void flush_saida(LiaChat *c)
{
    if (c->buffer_saida->len == 0) return;
    if (c->buffer_saida->str[c->buffer_saida->len - 1] == '\n')
        g_string_truncate(c->buffer_saida, c->buffer_saida->len - 1);
    if (c->term) {
        term_imprimir(c->term, c->buffer_saida->str);
    } else {
        fputs(c->buffer_saida->str, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    g_string_truncate(c->buffer_saida, 0);
}

/* ---- estilo: só no terminal interativo com cor; fora dele, texto puro ---- */
typedef enum { EST_NORMAL, EST_FRACO, EST_OK, EST_ERRO, EST_AVISO, EST_FORTE } Estilo;

static const char *sgr(LiaChat *c, Estilo e)
{
    if (!c->term || !term_tem_cor(c->term)) return "";
    switch (e) {
    case EST_FRACO: return "\033[2m";
    case EST_OK: return "\033[32m";
    case EST_ERRO: return "\033[31m";
    case EST_AVISO: return "\033[33m";
    case EST_FORTE: return "\033[1m";
    default: return "";
    }
}
static const char *sgr0(LiaChat *c) { return c->term && term_tem_cor(c->term) ? "\033[0m" : ""; }

/* texto vindo do daemon/modelo/sistema nunca leva caractere de controle ao
 * terminal (ESC de um nome de processo, de um texto de modelo...): só
 * '\n' e '\t' passam. Devolve cópia (g_free). */
static char *sanear(const char *s)
{
    GString *out = g_string_new(NULL);
    for (const char *p = s ? s : ""; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7f) g_string_append_c(out, '?');
        else g_string_append_c(out, (char)ch);
    }
    return g_string_free(out, FALSE);
}

static void historico_adicionar(LiaChat *c, const char *papel, const char *texto)
{
    if (!c->historico_conversa || !texto || !*texto) return;
    char *seguro = sanear(texto);
    glong n_chars = g_utf8_strlen(seguro, -1);
    if (n_chars > 1200) {
        char *curto = g_utf8_substring(seguro, 0, 1200);
        g_free(seguro);
        seguro = g_strconcat(curto, " …", NULL);
        g_free(curto);
    }
    g_string_append_printf(c->historico_conversa, "%s: %s\n", papel, seguro);
    g_free(seguro);
    if (c->historico_conversa->len > 8000) {
        gsize descartar = c->historico_conversa->len - 6000;
        char *quebra = memchr(c->historico_conversa->str + descartar, '\n',
                              c->historico_conversa->len - descartar);
        if (quebra) g_string_erase(c->historico_conversa, 0,
                                   (gssize)(quebra - c->historico_conversa->str) + 1);
    }
}

/* ---- fase atual -> linha de status (só estados operacionais observáveis) ---- */
static void definir_fase(LiaChat *c, Fase f)
{
    gboolean continua = c->fase != FASE_OCIOSO && f != FASE_OCIOSO; /* mesmo turno: o relógio não reinicia */
    if (f != FASE_OCIOSO) c->ocultar_status_pronto = FALSE;
    c->fase = f;
    if (!c->term) return;
    char *txt = NULL;
    switch (f) {
    case FASE_CONSULTANDO_MODELO: txt = g_strdup("consultando modelo"); break;
    case FASE_VALIDANDO_PLANO: txt = g_strdup_printf("validando plano%s%s", c->ferramenta_turno ? " · " : "", c->ferramenta_turno ? c->ferramenta_turno : ""); break;
    case FASE_AGUARDANDO_APROVACAO: txt = g_strdup_printf("aguardando aprovação%s%s — responda s ou n", c->ferramenta_turno ? " · " : "", c->ferramenta_turno ? c->ferramenta_turno : ""); break;
    case FASE_EXECUTANDO:
        if (c->tarefa_ativa) {
            int feitos = 0;
            const char *atual = NULL;
            for (int i = 0; i < c->tarefa_n; i++) {
                if (!strcmp(c->tarefa_estado[i], "executando")) { if (!atual) atual = c->tarefa_ferramenta[i]; }
                else if (strcmp(c->tarefa_estado[i], "aguardando")) feitos++;
            }
            const char *fase = c->tarefa_fase ? c->tarefa_fase : "executando";
            if (!strcmp(fase, "verificando")) txt = g_strdup_printf("verificando critérios · %d/%d passos", feitos, c->tarefa_n);
            else if (!strcmp(fase, "replanejando")) txt = g_strdup_printf("replanejando · %d/%d passos", feitos, c->tarefa_n);
            else txt = g_strdup_printf("coletando dados · %d/%d passos%s%s", feitos, c->tarefa_n, atual ? " · " : "", atual ? atual : "");
        } else {
            txt = g_strdup_printf("executando leitura%s%s", c->ferramenta_turno ? " · " : "", c->ferramenta_turno ? c->ferramenta_turno : "");
        }
        break;
    case FASE_GERANDO_RESPOSTA: txt = g_strdup(c->tarefa_objetivo ? "finalizando · comparando evidências" : "gerando resposta final"); break;
    default:
        txt = c->ocultar_status_pronto ? g_strdup("") :
              g_strdup_printf("conectado · pronto · %s · /help",
                              c->autopilot ? "leituras seguras automáticas" : "modo manual (toda ferramenta pede /plano)");
        break;
    }
    gboolean ativo = f != FASE_OCIOSO && f != FASE_AGUARDANDO_APROVACAO;
    term_definir_status(c->term, txt, ativo, continua);
    term_definir_prompt(c->term, f == FASE_AGUARDANDO_APROVACAO ? "aprovar? [s/N] " : "lia> ");
    g_free(txt);
}

static void ms_legivel(char *dst, gsize cap, gint64 ms);
static const char *campo_str(JsonObject *obj, const char *chave);
static gboolean campo_bool(JsonObject *obj, const char *chave, gboolean padrao);
static char *json_para_texto(JsonNode *n);
static JsonObject *campo_obj(JsonObject *obj, const char *chave);
static gint64 campo_int(JsonObject *obj, const char *chave, gint64 padrao);
static char *lista_str_para_texto(JsonObject *obj, const char *chave);

static void tarefa_ui_limpar(LiaChat *c)
{
    for (int i = 0; i < TAREFA_MAX_PASSOS_UI; i++) {
        g_clear_pointer(&c->tarefa_ferramenta[i], g_free);
        g_clear_pointer(&c->tarefa_estado[i], g_free);
        c->tarefa_ms[i] = 0;
        c->tarefa_replano[i] = FALSE;
    }
    c->tarefa_n = 0;
    c->tarefa_ativa = FALSE;
    g_clear_pointer(&c->tarefa_fase, g_free);
    if (c->term) term_definir_painel(c->term, NULL);
}

/* carrega a lista de passos a partir do array "passos" de um plano */
static void tarefa_ui_carregar(LiaChat *c, JsonObject *plano)
{
    if (!plano || !json_object_has_member(plano, "passos")) return;
    JsonNode *n = json_object_get_member(plano, "passos");
    if (JSON_NODE_TYPE(n) != JSON_NODE_ARRAY) return;
    JsonArray *arr = json_node_get_array(n);
    int total = (int)MIN(json_array_get_length(arr), (guint)TAREFA_MAX_PASSOS_UI);
    for (int i = 0; i < total; i++) {
        JsonNode *el = json_array_get_element(arr, (guint)i);
        if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) continue;
        JsonObject *o = json_node_get_object(el);
        char *params = json_object_has_member(o, "parametros") ? json_para_texto(json_object_get_member(o, "parametros")) : g_strdup("{}");
        const char *f = campo_str(o, "ferramenta");
        g_free(c->tarefa_ferramenta[i]);
        c->tarefa_ferramenta[i] = strcmp(params, "{}") ? g_strdup_printf("%s %s", f ? f : "?", params) : g_strdup(f ? f : "?");
        g_free(params);
        if (campo_str(o, "estado") || !c->tarefa_estado[i]) {
            g_free(c->tarefa_estado[i]);
            c->tarefa_estado[i] = g_strdup(campo_str(o, "estado") ? campo_str(o, "estado") : "aguardando");
        }
        c->tarefa_replano[i] = campo_bool(o, "de_replanejamento", FALSE);
        if (json_object_has_member(o, "tempo_execucao_ms")) c->tarefa_ms[i] = campo_int(o, "tempo_execucao_ms", 0);
    }
    c->tarefa_n = total;
    const char *obj = campo_str(plano, "objetivo");
    if (obj) { g_free(c->tarefa_objetivo); c->tarefa_objetivo = sanear(obj); }
}

/* "  [2/4] sistema.processos_top_cpu ........ executando" */
static void tarefa_ui_linha(LiaChat *c, GString *out, int i, gboolean cor)
{
    const char *e = c->tarefa_estado[i] ? c->tarefa_estado[i] : "aguardando";
    char *nome = g_strdup_printf("[%d/%d] %s%s ", i + 1, c->tarefa_n, c->tarefa_ferramenta[i] ? c->tarefa_ferramenta[i] : "?",
                                 c->tarefa_replano[i] ? " (replanejamento)" : "");
    glong larg = g_utf8_strlen(nome, -1);
    const char *rotulo = !strcmp(e, "sucesso") ? "concluído" : e;
    Estilo est = !strcmp(e, "sucesso") ? EST_OK : !strcmp(e, "executando") ? EST_FORTE :
                 (!strcmp(e, "falha") || !strcmp(e, "cancelado")) ? EST_ERRO : EST_FRACO;
    g_string_append_printf(out, "  %s", nome);
    for (glong k = larg; k < 58; k++) g_string_append_c(out, '.');
    g_string_append_printf(out, " %s%s%s", cor ? sgr(c, est) : "", rotulo, cor ? sgr0(c) : "");
    if (c->tarefa_ms[i] > 0 || !strcmp(e, "sucesso")) {
        char t[32];
        ms_legivel(t, sizeof t, c->tarefa_ms[i]);
        g_string_append_printf(out, " (%s)", t);
    }
    g_free(nome);
}

/* painel vivo da tarefa (acima da linha de status): objetivo + passos */
static void tarefa_ui_painel(LiaChat *c)
{
    if (!c->term) return;
    if (!c->tarefa_ativa) { term_definir_painel(c->term, NULL); return; }
    GString *p = g_string_new(NULL);
    g_string_append_printf(p, "%sObjetivo:%s %s\n", sgr(c, EST_FORTE), sgr0(c), c->tarefa_objetivo ? c->tarefa_objetivo : "");
    for (int i = 0; i < c->tarefa_n; i++) {
        tarefa_ui_linha(c, p, i, TRUE);
        g_string_append_c(p, '\n');
    }
    term_definir_painel(c->term, p->str);
    g_string_free(p, TRUE);
}

static void ms_legivel(char *dst, gsize cap, gint64 ms)
{
    if (ms < 1000) g_snprintf(dst, cap, "%" G_GINT64_FORMAT " ms", ms);
    else g_snprintf(dst, cap, "%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT " s", ms / 1000, (ms % 1000) / 100);
}

/* fim do turno (resposta final, erro, cancelamento, ou proposta que ficou
 * aguardando /plano): volta ao estado ocioso; com 'concluido', registra os
 * tempos reais de cada etapa. */
static void encerrar_turno(LiaChat *c, gboolean concluido)
{
    c->ocultar_status_pronto = c->turno_web;
    if (concluido && c->turno_inicio_us && !c->turno_web) {
        gint64 total = (g_get_monotonic_time() - c->turno_inicio_us) / 1000;
        char t[32], a[32], b[32], d[32];
        ms_legivel(t, sizeof t, total);
        GString *det = g_string_new(NULL);
        if (c->t_proposta_ms >= 0) { ms_legivel(a, sizeof a, c->t_proposta_ms); g_string_append_printf(det, "modelo/proposta %s", a); }
        if (c->t_execucao_ms >= 0) { ms_legivel(b, sizeof b, c->t_execucao_ms); g_string_append_printf(det, "%sferramenta %s", det->len ? " · " : "", b); }
        if (c->t_resposta_ms >= 0) { ms_legivel(d, sizeof d, c->t_resposta_ms); g_string_append_printf(det, "%sresposta final %s", det->len ? " · " : "", d); }
        if (c->term)
            saida_linha(c, "%s  ✓ concluído em %s%s%s%s%s", sgr(c, EST_FRACO), t, det->len ? " (" : "", det->str, det->len ? ")" : "", sgr0(c));
        else
            saida_linha(c, "[concluído] total=%" G_GINT64_FORMAT "ms proposta=%" G_GINT64_FORMAT "ms execucao=%" G_GINT64_FORMAT "ms resposta=%" G_GINT64_FORMAT "ms",
                        total, c->t_proposta_ms, c->t_execucao_ms, c->t_resposta_ms);
        g_string_free(det, TRUE);
    }
    c->turno_espera_resposta = FALSE;
    c->turno_web = FALSE;
    c->turno_inicio_us = 0;
    g_clear_pointer(&c->ferramenta_turno, g_free);
    tarefa_ui_limpar(c);
    g_clear_pointer(&c->tarefa_objetivo, g_free);
    definir_fase(c, FASE_OCIOSO);
}

static const char *campo_str(JsonObject *obj, const char *chave)
{
    if (!obj || !json_object_has_member(obj, chave)) return NULL;
    JsonNode *n = json_object_get_member(obj, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING) return NULL;
    return json_node_get_string(n);
}

static gboolean campo_bool(JsonObject *obj, const char *chave, gboolean padrao)
{
    if (!obj || !json_object_has_member(obj, chave)) return padrao;
    JsonNode *n = json_object_get_member(obj, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_BOOLEAN) return padrao;
    return json_node_get_boolean(n);
}

static gint64 campo_int(JsonObject *obj, const char *chave, gint64 padrao)
{
    if (!obj || !json_object_has_member(obj, chave)) return padrao;
    JsonNode *n = json_object_get_member(obj, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return padrao;
    GType t = json_node_get_value_type(n);
    if (t != G_TYPE_INT64 && t != G_TYPE_INT && t != G_TYPE_DOUBLE) return padrao;
    return json_node_get_int(n);
}

static char *json_para_texto(JsonNode *n)
{
    if (!n) return g_strdup("null");
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, n);
    char *s = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    return s ? s : g_strdup("null");
}

static char *lista_str_para_texto(JsonObject *obj, const char *chave)
{
    GString *out = g_string_new(NULL);
    if (obj && json_object_has_member(obj, chave)) {
        JsonNode *n = json_object_get_member(obj, chave);
        if (JSON_NODE_TYPE(n) == JSON_NODE_ARRAY) {
            JsonArray *arr = json_node_get_array(n);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                if (i) g_string_append(out, ", ");
                JsonNode *el = json_array_get_element(arr, i);
                if (JSON_NODE_TYPE(el) == JSON_NODE_VALUE && json_node_get_value_type(el) == G_TYPE_STRING)
                    g_string_append(out, json_node_get_string(el));
            }
        }
    }
    if (out->len == 0) g_string_append(out, "(nenhuma)");
    return g_string_free(out, FALSE);
}

/* --------------------------------------------------------------- envio --- */

/* Monta e envia; devolve o id gerado (quem chamar não libera 'dados').
 * NULL se recusado localmente (sem conexão, ou lia_sessao_validar_envio
 * recusou) ou se a escrita falhou — nesses casos nada foi enviado. */
/* pular_validacao_local: gui/protocolo.c's lia_sessao_validar_envio só
 * conhece pendências rastreadas por execucao_id (populado só a partir de
 * 'progresso'/'saida'/'resultado') — nunca um plano ainda aguardando
 * aprovação (sem execucao_id ainda). Cancelar ESSE plano é um uso legítimo
 * do protocolo (sessiond.c, tratar_cancelar) que a biblioteca
 * compartilhada simplesmente não modela; lia-chat já sabe, com certeza,
 * se há algo para cancelar (c->plano_id_pendente/execucao_id_pendente) —
 * então pula só essa checagem redundante e incompleta, nunca a ignora
 * por conta própria sem ter verificado nada. */
static char *enviar(LiaChat *c, LiaMsgTipo tipo, JsonBuilder *dados,
                     const char * const *canais, size_t n_canais, gboolean pular_validacao_local)
{
    if (!pular_validacao_local) {
        LiaEnvioErro erro = lia_sessao_validar_envio(&c->sessao, tipo, canais, n_canais);
        if (erro != LIA_ENVIO_OK) {
            saida_linha(c, "[local] envio recusado localmente (código %d)", (int)erro);
            return NULL;
        }
    }
    if (!c->conexao || !c->saida) {
        saida_linha(c, "[local] sem conexão com o daemon");
        return NULL;
    }
    char *id = lia_novo_id("chat");
    char *linha = lia_msg_montar(tipo, id, NULL, dados);
    GError *gerro = NULL;
    gboolean ok = g_output_stream_write_all(c->saida, linha, strlen(linha), NULL, NULL, &gerro);
    g_free(linha);
    if (!ok) {
        saida_linha(c, "[local] falha ao escrever no socket: %s", gerro ? gerro->message : "?");
        if (gerro) g_error_free(gerro);
        g_free(id);
        return NULL;
    }
    g_free(c->ultimo_id_enviado);
    c->ultimo_id_enviado = g_strdup(id);
    /* gui/protocolo.c só registra em ids_vistos o que é RECEBIDO; mas o
     * daemon responde 'plano'/'proposta'/'erro' com em_resposta_a = o id
     * que NÓS enviamos (sessiond.c, enviar_plano/enviar_proposta/
     * enviar_erro). Sem isto, lia_sessao_receber nunca reconheceria essa
     * referência — registra aqui para a checagem de "resposta conhecida"
     * valer de verdade, sem alterar a biblioteca compartilhada. */
    g_hash_table_add(c->sessao.ids_vistos, g_strdup(id));
    return id;
}

/* Respostas de modelo real (Ollama/llama.cpp) em CPU podem levar dezenas
 * de segundos (medido: ~34s para um prompt típico da Lia contra
 * llama3.2:3b nesta máquina) — sem isto, o terminal fica em silêncio
 * total entre "pergunta enviada" e a resposta/erro final, parecendo
 * travado mesmo quando está só funcionando normalmente. */
#define LIA_PROGRESSO_PERGUNTA_INTERVALO_S 5

static void parar_progresso_pergunta(LiaChat *c)
{
    if (c->timer_progresso_pergunta) {
        g_source_remove(c->timer_progresso_pergunta);
        c->timer_progresso_pergunta = 0;
    }
}

static gboolean ao_timer_progresso_pergunta(gpointer dados)
{
    LiaChat *c = dados;
    gint64 decorrido_s = (g_get_monotonic_time() - c->pergunta_enviada_us) / 1000000;
    /* saida_linha só acumula no buffer — sem este flush_saida explícito,
     * nada disto aparece na tela em tempo real; fica só empilhado até a
     * próxima mensagem do daemon chegar e esvaziar tudo de uma vez junto
     * com a resposta final. Era exatamente esse bug que fazia o "[local]
     * ainda aguardando..." nunca aparecer ao vivo — só tudo junto no
     * final, dando a impressão de nenhum progresso real. Mensagem nunca
     * expõe raciocínio do modelo — só o estado observável "consultando",
     * igual ao que aparece para qualquer execução de ferramenta. */
    saida_linha(c, "[local] %s... (%" G_GINT64_FORMAT "s)",
                c->fase == FASE_GERANDO_RESPOSTA ? "gerando resposta final" : "consultando modelo", decorrido_s);
    flush_saida(c);
    return G_SOURCE_CONTINUE;
}

static void iniciar_progresso_pergunta(LiaChat *c)
{
    parar_progresso_pergunta(c);
    c->pergunta_enviada_us = g_get_monotonic_time();
    /* no terminal interativo a linha de status já mostra a fase, o
     * indicador de atividade e o tempo decorrido, atualizados ao vivo —
     * linhas periódicas no histórico seriam só ruído. Fora dele (pipe,
     * script), continuam: é o único sinal de vida disponível. */
    if (!c->term)
        c->timer_progresso_pergunta = g_timeout_add_seconds(LIA_PROGRESSO_PERGUNTA_INTERVALO_S, ao_timer_progresso_pergunta, c);
}

static void enviar_perguntar(LiaChat *c, const char *mensagem)
{
    if (c->pergunta_em_andamento || c->fase != FASE_OCIOSO) {
        saida_linha(c, "[local] ainda há um pedido em andamento — aguarde a resposta ou cancele (Ctrl-C ou /cancelar)");
        return;
    }
    if (strlen(mensagem) > LIA_PROMPT_LIMITE_PEDIDO_BYTES) {
        saida_linha(c, "[local] mensagem acima do limite de %d bytes — não enviada", LIA_PROMPT_LIMITE_PEDIDO_BYTES);
        return;
    }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "mensagem");
    json_builder_add_string_value(b, mensagem);
    if (c->historico_conversa && c->historico_conversa->len) {
        json_builder_set_member_name(b, "contexto");
        json_builder_add_string_value(b, c->historico_conversa->str);
    }
    if (c->arquivos->len > 0) {
        json_builder_set_member_name(b, "arquivos");
        json_builder_begin_array(b);
        for (guint i = 0; i < c->arquivos->len; i++) {
            ArquivoAnexo *a = g_ptr_array_index(c->arquivos, i);
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "caminho");
            json_builder_add_string_value(b, a->caminho);
            json_builder_set_member_name(b, "conteudo");
            json_builder_add_string_value(b, a->conteudo);
            json_builder_end_object(b);
        }
        json_builder_end_array(b);
    }
    json_builder_end_object(b);
    char *id = enviar(c, LIA_MSG_PERGUNTAR, b, NULL, 0, FALSE);
    g_object_unref(b);
    if (id) {
        historico_adicionar(c, "Usuário", mensagem);
        c->pergunta_em_andamento = TRUE;
        c->pergunta_abandonada_localmente = FALSE;
        c->turno_inicio_us = g_get_monotonic_time();
        c->t_proposta_ms = c->t_execucao_ms = c->t_resposta_ms = -1;
        iniciar_progresso_pergunta(c);
        definir_fase(c, FASE_CONSULTANDO_MODELO);
        if (!c->term)
            saida_linha(c, "[local] pergunta enviada (id da mensagem: %s)", id);
        g_free(id);
    }
}

static void enviar_solicitar_da_proposta(LiaChat *c, const char *modo)
{
    if (!c->proposta_pendente) {
        saida_linha(c, "[local] nenhuma proposta de ferramenta pendente");
        return;
    }
    PropostaPendente *p = c->proposta_pendente;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    if (p->plano_id) {
        /* tarefa: só a referência ao plano validado e o hash dele — o
         * conteúdo do plano fica no daemon e não é reenviado (não há como
         * alterá-lo daqui) */
        json_builder_set_member_name(b, "plano_id");
        json_builder_add_string_value(b, p->plano_id);
        json_builder_set_member_name(b, "hash");
        json_builder_add_string_value(b, p->plano_hash ? p->plano_hash : "");
    } else {
        json_builder_set_member_name(b, "ferramenta");
        json_builder_add_string_value(b, p->ferramenta);
        json_builder_set_member_name(b, "parametros");
        JsonParser *pp = json_parser_new();
        if (p->parametros_json && json_parser_load_from_data(pp, p->parametros_json, -1, NULL))
            json_builder_add_value(b, json_node_copy(json_parser_get_root(pp)));
        else {
            json_builder_begin_object(b);
            json_builder_end_object(b);
        }
        g_object_unref(pp);
    }
    json_builder_set_member_name(b, "modo");
    json_builder_add_string_value(b, modo);
    json_builder_set_member_name(b, "origem_modelo");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "pergunta_id");
    json_builder_add_string_value(b, p->pergunta_id);
    if (p->justificativa) {
        json_builder_set_member_name(b, "justificativa");
        json_builder_add_string_value(b, p->justificativa);
    }
    json_builder_end_object(b);
    char *id = enviar(c, LIA_MSG_SOLICITAR, b, NULL, 0, FALSE);
    g_object_unref(b);
    if (id) {
        /* a execução vem de uma proposta: o daemon fecha o turno com uma
         * 'resposta' final (resumo fundamentado, ou determinística). */
        c->turno_espera_resposta = TRUE;
        g_free(c->ferramenta_turno);
        c->ferramenta_turno = g_strdup(p->ferramenta);
        if (!c->turno_inicio_us) {
            c->turno_inicio_us = g_get_monotonic_time();
            c->t_proposta_ms = c->t_execucao_ms = c->t_resposta_ms = -1;
        }
        definir_fase(c, FASE_VALIDANDO_PLANO);
    }
    g_free(id);
    if (!strcmp(modo, "simular"))
        saida_linha(c, "[local] nesta etapa, modo=\"simular\" não tem efeito distinto de \"executar\" no executor real — nenhuma ferramenta atual implementa simulação de verdade (ver docs/protocolo.md §8).");
    proposta_pendente_liberar(c->proposta_pendente);
    c->proposta_pendente = NULL;
}

static void tratar_decisao_plano(LiaChat *c, gboolean aprovar)
{
    if (!c->plano_id_pendente) {
        saida_linha(c, "[local] nenhum plano pendente para %s", aprovar ? "aprovar" : "rejeitar");
        return;
    }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "plano_id");
    json_builder_add_string_value(b, c->plano_id_pendente); /* exatamente o id recebido — nunca reconstruído */
    /* a decisão vale para ESTE plano: o hash que o daemon calculou do plano
     * apresentado vai junto, sem alteração */
    json_builder_set_member_name(b, "hash");
    json_builder_add_string_value(b, c->plano_hash_pendente ? c->plano_hash_pendente : "");
    json_builder_set_member_name(b, "decisao");
    json_builder_add_string_value(b, aprovar ? "aprovar" : "rejeitar");
    json_builder_set_member_name(b, "escopo");
    json_builder_add_string_value(b, "uma_vez");
    json_builder_end_object(b);
    char *id = enviar(c, LIA_MSG_APROVAR, b, NULL, 0, FALSE);
    g_object_unref(b);
    g_free(id);
    saida_linha(c, "[local] %s enviado para o plano %s", aprovar ? "aprovação" : "rejeição", c->plano_id_pendente);
    c->aguardando_decisao_plano = FALSE;
    if (aprovar) definir_fase(c, FASE_EXECUTANDO);
}

static void tratar_cancelar(LiaChat *c)
{
    /* 'plano_id_pendente' sozinho não basta: continua preenchido depois
     * de auto-despachado (sem aprovação) ou já aprovado — nesses casos o
     * daemon já saiu de "aguardando aprovação" e o que existe para
     * cancelar é a EXECUÇÃO, não o plano (sessiond.c, tratar_cancelar:
     * cancelar sem execucao_id só é válido em ESTADO_AGUARDANDO_APROVACAO). */
    if (c->plano_id_pendente && c->aguardando_decisao_plano) {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_end_object(b); /* sem execucao_id: cobre o plano aguardando aprovação */
        char *id = enviar(c, LIA_MSG_CANCELAR, b, NULL, 0, TRUE);
        g_object_unref(b);
        g_free(id);
        saida_linha(c, "[local] cancelamento do plano pendente enviado");
        return;
    }
    if (c->execucao_id_pendente) {
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "execucao_id");
        json_builder_add_string_value(b, c->execucao_id_pendente);
        json_builder_end_object(b);
        char *id = enviar(c, LIA_MSG_CANCELAR, b, NULL, 0, TRUE);
        g_object_unref(b);
        g_free(id);
        saida_linha(c, "[local] cancelamento da execução %s enviado", c->execucao_id_pendente);
        return;
    }
    if (c->pergunta_em_andamento || c->fase == FASE_GERANDO_RESPOSTA) {
        /* conversa com o modelo em andamento (a pergunta, ou o resumo
         * final): o daemon cancela a geração de verdade e confirma com um
         * 'erro' de código "cancelado" (docs/protocolo.md §10). */
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "pergunta");
        json_builder_add_boolean_value(b, TRUE);
        json_builder_end_object(b);
        char *id = enviar(c, LIA_MSG_CANCELAR, b, NULL, 0, TRUE);
        g_object_unref(b);
        g_free(id);
        saida_linha(c, "[local] cancelamento da %s enviado", c->pergunta_em_andamento ? "pergunta ao modelo" : "resposta final");
        return;
    }
    saida_linha(c, "[local] nada para cancelar");
}

/* ------------------------------------------------------- probe local --- */

static void probe_modelo(LiaChat *c, gboolean completo)
{
    if (c->modelo_assumido)
        g_setenv("LIA_OLLAMA_MODELO", c->modelo_assumido, TRUE);
    const ModeloBackend *m = !strcmp(c->backend_assumido, "ollama") ? modelo_ollama_obter() :
                              !strcmp(c->backend_assumido, "llama_cpp") ? modelo_llama_cpp_obter() :
                              modelo_mock_obter();
    char *detalhe = NULL;
    gboolean disp = m->disponivel(m, &detalhe);
    saida_linha(c, "backend assumido (local, não controla o daemon remotamente): %s", c->backend_assumido);
    if (completo)
        saida_linha(c, "modelo assumido: %s", c->modelo_assumido ? c->modelo_assumido : "(padrão do backend)");
    saida_linha(c, "disponibilidade: %s — %s", disp ? "sim" : "não", detalhe ? detalhe : "?");
    g_free(detalhe);
}

/* --------------------------------------------------------- impressão --- */

static void imprimir_plano(LiaChat *c, JsonObject *d)
{
    const char *ferramenta = campo_str(d, "ferramenta");
    const char *objetivo = campo_str(d, "objetivo");
    const char *justificativa = campo_str(d, "justificativa");
    const char *risco = campo_str(d, "risco");
    const char *reversivel = campo_str(d, "reversivel");
    const char *versao = campo_str(d, "versao_manifesto");
    gboolean requer_aprovacao = campo_bool(d, "requer_aprovacao", TRUE);
    gboolean idempotente = campo_bool(d, "idempotente", FALSE);
    gint64 timeout_ms = campo_int(d, "timeout_padrao_ms", 0);
    char *capacidades = lista_str_para_texto(d, "capacidades");
    char *efeitos = lista_str_para_texto(d, "efeitos");
    char *parametros = NULL;
    if (d && json_object_has_member(d, "parametros_normalizados"))
        parametros = json_para_texto(json_object_get_member(d, "parametros_normalizados"));

    saida_linha(c, "");
    saida_linha(c, "Plano pendente");
    saida_linha(c, "  Ferramenta:  %s (versão do manifesto: %s)", ferramenta ? ferramenta : "?", versao ? versao : "?");
    saida_linha(c, "  Objetivo:    %s", (objetivo && *objetivo) ? objetivo : "(não informado)");
    if (justificativa) saida_linha(c, "  Justificativa: %s", justificativa);
    saida_linha(c, "  Risco:       %s", risco ? risco : "?");
    saida_linha(c, "  Capacidades: %s", capacidades);
    saida_linha(c, "  Parâmetros:  %s", parametros ? parametros : "{}");
    saida_linha(c, "  Efeitos:     %s", efeitos);
    saida_linha(c, "  Idempotente: %s", idempotente ? "sim" : "não");
    saida_linha(c, "  Reversão:    %s", reversivel ? reversivel : "?");
    saida_linha(c, "  Timeout:     %" G_GINT64_FORMAT " ms", timeout_ms);
    saida_linha(c, "  Requer aprovação: %s", requer_aprovacao ? "sim" : "não");
    g_free(capacidades);
    g_free(efeitos);
    g_free(parametros);

    if (requer_aprovacao) {
        saida_linha(c, "");
        saida_linha(c, "Aprovar este plano? [s/N] ");
    }
}

static void imprimir_ajuda(LiaChat *c)
{
    saida_linha(c,
        "Comandos:\n"
        "  /help                      esta ajuda\n"
        "  /status                    estado da sessão (conexão, backend, pendências)\n"
        "  /modelo                    backend/modelo assumidos e disponibilidade do Ollama\n"
        "  /backend mock|ollama       muda a suposição local (não troca o backend do daemon)\n"
        "  /arquivos listar           lista os arquivos anexados\n"
        "  /arquivo adicionar CAMINHO anexa um arquivo (dado não confiável, nunca instrução)\n"
        "  /arquivo remover INDICE    remove o arquivo pelo índice mostrado em /arquivos listar\n"
        "  /plano                     reexibe o plano pendente, ou solicita a última ferramenta proposta (modo executar)\n"
        "  /simular                   como /plano, mas pedindo modo=\"simular\"\n"
        "  /aprovar                   aprova o plano pendente (envia o plano_id recebido)\n"
        "  /rejeitar                  rejeita o plano pendente\n"
        "  /cancelar                  cancela o plano/execução/pergunta pendente (ou Ctrl-C)\n"
        "  /autopilot read-only|manual  leituras seguras automáticas, ou toda ferramenta pede /plano\n"
        "  /dados                     mostra o JSON bruto do último resultado\n"
        "  /limpar                    limpa a tela\n"
        "  /sair                      encerra (ou Ctrl-D)\n"
        "\n"
        "Qualquer outra linha é enviada ao modelo como uma pergunta — exceto quando um\n"
        "plano está pendente: nesse caso a linha é lida como [s/N] (padrão: rejeitar).\n"
        "\n"
        "Piloto automático (LIA_CHAT_AUTOPILOT=read-only, padrão): quando o daemon valida a\n"
        "proposta como LEITURA PURA, ela é solicitada e executada sem parar, e a resposta final\n"
        "cita os dados reais. Tudo que grava, administra ou toca hardware continua exigindo\n"
        "aprovação explícita — em qualquer modo.");
}

static void imprimir_status(LiaChat *c)
{
    saida_linha(c, "conexão: %s (%s)", c->conexao ? "conectado" : "desconectado", c->socket_caminho);
    probe_modelo(c, TRUE);
    saida_linha(c, "pergunta em andamento: %s", c->pergunta_em_andamento ?
        (c->pergunta_abandonada_localmente ? "sim (abandonada localmente)" : "sim") : "nenhuma");
    saida_linha(c, "proposta de ferramenta pendente: %s",
        c->proposta_pendente ? c->proposta_pendente->ferramenta : "nenhuma");
    saida_linha(c, "plano pendente: %s", c->plano_id_pendente ?
        (c->ferramenta_plano_pendente ? c->ferramenta_plano_pendente : c->plano_id_pendente) : "nenhum");
    saida_linha(c, "execução pendente: %s", c->execucao_id_pendente ? c->execucao_id_pendente : "nenhuma");
    saida_linha(c, "arquivos anexados: %u", c->arquivos->len);
    saida_linha(c, "piloto automático: %s", c->autopilot ? "read-only (leituras seguras executam sozinhas)" : "manual (toda ferramenta pede /plano)");
}

/* ------------------------------------------------------- comando/arquivo */

static gboolean arquivo_adicionar(LiaChat *c, const char *caminho)
{
    if (c->arquivos->len >= LIA_PROMPT_LIMITE_ARQUIVOS_TOTAL) {
        saida_linha(c, "[local] limite de %d arquivos anexados — remova algum antes (/arquivo remover)", LIA_PROMPT_LIMITE_ARQUIVOS_TOTAL);
        return FALSE;
    }
    struct stat st_link;
    if (lstat(caminho, &st_link) != 0) {
        saida_linha(c, "[local] não foi possível acessar '%s': %s", caminho, g_strerror(errno));
        return FALSE;
    }
    if (S_ISLNK(st_link.st_mode)) {
        saida_linha(c, "[local] '%s' é um link simbólico — informe o caminho real, lia-chat não segue links", caminho);
        return FALSE;
    }
    if (S_ISDIR(st_link.st_mode)) {
        saida_linha(c, "[local] '%s' é um diretório — só arquivos regulares podem ser anexados", caminho);
        return FALSE;
    }
    if (!S_ISREG(st_link.st_mode)) {
        saida_linha(c, "[local] '%s' não é um arquivo regular", caminho);
        return FALSE;
    }

    gsize total_atual = 0;
    for (guint i = 0; i < c->arquivos->len; i++)
        total_atual += ((ArquivoAnexo *)g_ptr_array_index(c->arquivos, i))->tamanho;

    GError *erro = NULL;
    char *conteudo = NULL;
    gsize tamanho = 0;
    if (!g_file_get_contents(caminho, &conteudo, &tamanho, &erro)) {
        saida_linha(c, "[local] falha ao ler '%s': %s", caminho, erro ? erro->message : "?");
        if (erro) g_error_free(erro);
        return FALSE;
    }
    if (tamanho > LIA_PROMPT_LIMITE_ARQUIVO_BYTES) {
        saida_linha(c, "[local] '%s' tem %zu bytes, acima do limite de %d por arquivo — não anexado",
                    caminho, tamanho, LIA_PROMPT_LIMITE_ARQUIVO_BYTES);
        g_free(conteudo);
        return FALSE;
    }
    if (total_atual + tamanho > LIA_PROMPT_LIMITE_CONTEXTO_BYTES) {
        saida_linha(c, "[local] anexar '%s' passaria do limite total de %d bytes de contexto — não anexado",
                    caminho, LIA_PROMPT_LIMITE_CONTEXTO_BYTES);
        g_free(conteudo);
        return FALSE;
    }

    ArquivoAnexo *a = g_new0(ArquivoAnexo, 1);
    a->caminho = g_strdup(caminho);
    a->conteudo = conteudo; /* já é NUL-terminado por g_file_get_contents */
    a->tamanho = tamanho;
    g_ptr_array_add(c->arquivos, a);
    saida_linha(c, "[local] anexado: %s (%zu bytes, índice %u)", caminho, tamanho, c->arquivos->len);
    return TRUE;
}

static void arquivos_listar(LiaChat *c)
{
    if (c->arquivos->len == 0) { saida_linha(c, "(nenhum arquivo anexado)"); return; }
    for (guint i = 0; i < c->arquivos->len; i++) {
        ArquivoAnexo *a = g_ptr_array_index(c->arquivos, i);
        saida_linha(c, "  %u. %s (%zu bytes)", i + 1, a->caminho, a->tamanho);
    }
}

static void arquivo_remover(LiaChat *c, const char *indice_str)
{
    char *fim = NULL;
    long idx = strtol(indice_str, &fim, 10);
    if (fim == indice_str || idx < 1 || (guint)idx > c->arquivos->len) {
        saida_linha(c, "[local] índice inválido: '%s' (use /arquivos listar)", indice_str);
        return;
    }
    ArquivoAnexo *a = g_ptr_array_index(c->arquivos, (guint)(idx - 1));
    saida_linha(c, "[local] removido: %s", a->caminho);
    g_ptr_array_remove_index(c->arquivos, (guint)(idx - 1));
}

/* ----------------------------------------------- recepção do daemon --- */

static void limpar_pendencias_de_execucao(LiaChat *c)
{
    g_free(c->plano_id_pendente); c->plano_id_pendente = NULL;
    g_free(c->ferramenta_plano_pendente); c->ferramenta_plano_pendente = NULL;
    g_free(c->plano_hash_pendente); c->plano_hash_pendente = NULL;
    if (c->plano_dados_pendente) { json_object_unref(c->plano_dados_pendente); c->plano_dados_pendente = NULL; }
    c->aguardando_decisao_plano = FALSE;
    g_free(c->execucao_id_pendente); c->execucao_id_pendente = NULL;
}

static void tratar_msg_plano(LiaChat *c, const LiaMsg *m)
{
    limpar_pendencias_de_execucao(c);
    const char *plano_id = campo_str(m->dados, "plano_id");
    const char *ferramenta = campo_str(m->dados, "ferramenta");
    c->plano_id_pendente = g_strdup(plano_id ? plano_id : "");
    c->ferramenta_plano_pendente = g_strdup(ferramenta ? ferramenta : "");
    c->plano_dados_pendente = json_object_ref(m->dados);
    c->aguardando_decisao_plano = campo_bool(m->dados, "requer_aprovacao", TRUE);
    c->plano_hash_pendente = g_strdup(campo_str(m->dados, "hash") ? campo_str(m->dados, "hash") : "");
    if (campo_bool(m->dados, "tarefa", FALSE)) {
        /* plano de vários passos: mostra objetivo, passos, risco e o hash
         * ao qual a execução (e a aprovação, se houver) fica presa */
        tarefa_ui_limpar(c);
        tarefa_ui_carregar(c, m->dados);
        c->tarefa_ativa = TRUE;
        const char *exec = campo_str(m->dados, "execucao_id");
        if (exec) { g_free(c->execucao_id_pendente); c->execucao_id_pendente = g_strdup(exec); }
        const char *risco = campo_str(m->dados, "risco"), *hash = c->plano_hash_pendente;
        gint64 orc_ms = campo_int(m->dados, "orcamento_tempo_ms", 0), orc_p = campo_int(m->dados, "orcamento_passos", 0);
        if (c->term) {
            saida_linha(c, "%sPlano da tarefa%s — %s", sgr(c, EST_FORTE), sgr0(c), c->tarefa_objetivo ? c->tarefa_objetivo : "");
            saida_linha(c, "%s  %d passos · risco %s · %s · orçamento: %" G_GINT64_FORMAT " passos, %" G_GINT64_FORMAT " s · hash %.12s…%s",
                        sgr(c, EST_FRACO), c->tarefa_n, risco ? risco : "?",
                        c->aguardando_decisao_plano ? "EXIGE APROVAÇÃO" : "somente leitura, sem aprovação",
                        orc_p, orc_ms / 1000, hash, sgr0(c));
        } else {
            saida_linha(c, "[tarefa] objetivo: %s", c->tarefa_objetivo ? c->tarefa_objetivo : "");
            saida_linha(c, "[tarefa] plano: %d passos, risco=%s, requer_aprovacao=%s, hash=%s", c->tarefa_n, risco ? risco : "?",
                        c->aguardando_decisao_plano ? "sim" : "nao", hash);
            for (int i = 0; i < c->tarefa_n; i++)
                saida_linha(c, "[tarefa]   [%d/%d] %s", i + 1, c->tarefa_n, c->tarefa_ferramenta[i]);
        }
        if (c->aguardando_decisao_plano) {
            if (c->term) {
                GString *l = g_string_new(NULL);
                for (int i = 0; i < c->tarefa_n; i++) { tarefa_ui_linha(c, l, i, FALSE); g_string_append_c(l, '\n'); }
                saida_linha(c, "%s", l->str);
                g_string_free(l, TRUE);
            }
            saida_linha(c, "Aprovar este plano? [s/N] ");
        }
        tarefa_ui_painel(c);
        definir_fase(c, c->aguardando_decisao_plano ? FASE_AGUARDANDO_APROVACAO : FASE_EXECUTANDO);
        return;
    }
    if (!c->ferramenta_turno && ferramenta) c->ferramenta_turno = g_strdup(ferramenta);
    if (c->term && !c->aguardando_decisao_plano) {
        /* leitura sem aprovação: uma linha basta (o plano completo continua
         * disponível com /plano enquanto a execução durar) */
        const char *risco = campo_str(m->dados, "risco");
        (void)ferramenta;
        saida_linha(c, "%s  ✓ plano validado pelo daemon: risco %s, somente leitura, sem aprovação%s",
                    sgr(c, EST_FRACO), risco ? risco : "?", sgr0(c));
    } else {
        imprimir_plano(c, m->dados);
        if (!c->aguardando_decisao_plano)
            saida_linha(c, "[local] sem aprovação necessária — executando");
    }
    definir_fase(c, c->aguardando_decisao_plano ? FASE_AGUARDANDO_APROVACAO : FASE_EXECUTANDO);
}

static void tratar_msg_progresso_ou_saida(LiaChat *c, const LiaMsg *m, gboolean progresso)
{
    const char *exec_id = campo_str(m->dados, "execucao_id");
    if (exec_id && !c->execucao_id_pendente)
        c->execucao_id_pendente = g_strdup(exec_id);
    JsonObject *tar = progresso ? campo_obj(m->dados, "tarefa") : NULL;
    if (tar) {
        /* evento de uma TAREFA: fase atual e, quando houver, a observação de um passo */
        const char *fase = campo_str(tar, "fase");
        g_free(c->tarefa_fase);
        c->tarefa_fase = g_strdup(fase ? fase : "executando");
        c->tarefa_ativa = TRUE;
        JsonObject *plano = campo_obj(tar, "plano");
        if (plano) tarefa_ui_carregar(c, plano); /* replanejamento: o plano cresceu */
        JsonObject *ps = campo_obj(tar, "passo");
        char *msg = sanear(campo_str(m->dados, "mensagem"));
        if (ps) {
            int i = (int)campo_int(ps, "indice", 0) - 1;
            const char *est = campo_str(ps, "estado");
            if (i >= 0 && i < TAREFA_MAX_PASSOS_UI && est) {
                if (i >= c->tarefa_n) c->tarefa_n = i + 1;
                g_free(c->tarefa_estado[i]);
                c->tarefa_estado[i] = g_strdup(est);
                c->tarefa_ms[i] = campo_int(ps, "tempo_execucao_ms", 0);
                if (!c->tarefa_ferramenta[i]) c->tarefa_ferramenta[i] = g_strdup(campo_str(ps, "ferramenta") ? campo_str(ps, "ferramenta") : "?");
                gboolean final = strcmp(est, "executando") && strcmp(est, "aguardando");
                if (final) {
                    char *ev = lista_str_para_texto(ps, "evidencias");
                    char *lim = lista_str_para_texto(ps, "limitacoes");
                    char *erro = campo_str(ps, "erro") ? sanear(campo_str(ps, "erro")) : NULL;
                    char *apres = campo_str(ps, "apresentacao") ? sanear(campo_str(ps, "apresentacao")) : NULL;
                    if (apres) g_strchomp(apres);
                    char t[32];
                    ms_legivel(t, sizeof t, c->tarefa_ms[i]);
                    gboolean ok = !strcmp(est, "sucesso");
                    if (c->term) {
                        saida_linha(c, "%s  %s [%d/%d] %s — %s em %s%s", sgr(c, ok ? EST_OK : EST_ERRO), ok ? "✓" : "✗", i + 1, c->tarefa_n,
                                    c->tarefa_ferramenta[i], ok ? "dados reais recebidos" : est, t, sgr0(c));
                        if (ok) saida_linha(c, "%s    evidências: %s%s", sgr(c, EST_FRACO), ev, sgr0(c));
                        if (strcmp(lim, "(nenhuma)")) saida_linha(c, "%s    limitações: %s%s", sgr(c, EST_AVISO), lim, sgr0(c));
                        if (erro) saida_linha(c, "%s    %s%s", sgr(c, EST_ERRO), erro, sgr0(c));
                        if (apres && *apres) { saida_linha(c, ""); saida_linha(c, "%s", apres); saida_linha(c, ""); }
                    } else {
                        saida_linha(c, "[passo %d/%d] %s: %s (%s) evidências: %s%s%s", i + 1, c->tarefa_n, c->tarefa_ferramenta[i], est, t, ev,
                                    erro ? " erro: " : "", erro ? erro : "");
                        if (strcmp(lim, "(nenhuma)")) saida_linha(c, "[passo %d/%d] limitações: %s", i + 1, c->tarefa_n, lim);
                        if (apres && *apres) saida_linha(c, "%s", apres);
                    }
                    g_free(ev); g_free(lim); g_free(erro); g_free(apres);
                } else if (!c->term) {
                    saida_linha(c, "[passo %d/%d] %s: iniciado", i + 1, c->tarefa_n, c->tarefa_ferramenta[i]);
                }
            }
        } else if (fase && !strcmp(fase, "replanejando")) {
            if (c->term) saida_linha(c, "%s  ↻ %s%s", sgr(c, EST_AVISO), msg, sgr0(c));
            else saida_linha(c, "[tarefa] replanejando: %s", msg);
        } else if (fase && !strcmp(fase, "verificando")) {
            if (!c->term) saida_linha(c, "[tarefa] verificando: %s", msg);
        }
        g_free(msg);
        tarefa_ui_painel(c);
        definir_fase(c, FASE_EXECUTANDO);
        return;
    }
    if (progresso) {
        double fracao = 0;
        if (json_object_has_member(m->dados, "fracao")) {
            JsonNode *n = json_object_get_member(m->dados, "fracao");
            if (JSON_NODE_TYPE(n) == JSON_NODE_VALUE) fracao = json_node_get_double(n);
        }
        saida_linha(c, "[progresso %.0f%%] %s", fracao * 100.0, campo_str(m->dados, "mensagem"));
    } else {
        const char *trecho = campo_str(m->dados, "trecho");
        if (trecho) saida_linha(c, "%s", trecho);
    }
}

static void tratar_msg_resultado(LiaChat *c, const LiaMsg *m)
{
    const char *estado = campo_str(m->dados, "estado");
    gint64 codigo = campo_int(m->dados, "codigo_saida", -1);
    const char *apresentacao = campo_str(m->dados, "apresentacao");
    gint64 exec_ms = campo_int(m->dados, "tempo_execucao_ms", -1);
    gboolean sucesso = estado && !strcmp(estado, "sucesso");
    c->t_execucao_ms = exec_ms;

    g_free(c->ultimo_dados_json);
    c->ultimo_dados_json = json_object_has_member(m->dados, "dados")
        ? json_para_texto(json_object_get_member(m->dados, "dados")) : NULL;
    char *apres = apresentacao ? sanear(apresentacao) : NULL;
    if (apres) g_strchomp(apres);
    if (!campo_bool(m->dados, "tarefa", FALSE) && sucesso && apres && *apres)
        historico_adicionar(c, "Lia", apres);

    if (campo_bool(m->dados, "tarefa", FALSE)) {
        /* resultado da TAREFA inteira: os dados de cada passo já foram
         * mostrados à medida que chegaram; aqui só o veredito do verificador */
        JsonObject *d = campo_obj(m->dados, "dados");
        tarefa_ui_carregar(c, campo_obj(d, "tarefa")); /* estado FINAL de cada passo, como o daemon registrou */
        c->turno_web = FALSE;
        for (int i = 0; i < c->tarefa_n; i++)
            if (c->tarefa_ferramenta[i] && g_str_has_prefix(c->tarefa_ferramenta[i], "web.") &&
                c->tarefa_estado[i] && !strcmp(c->tarefa_estado[i], "sucesso"))
                c->turno_web = TRUE;
        const char *veredito = campo_str(d, "veredito");
        char *aus = lista_str_para_texto(d, "evidencias_ausentes");
        char t[32];
        ms_legivel(t, sizeof t, exec_ms < 0 ? 0 : exec_ms);
        gboolean cancelada = estado && !strcmp(estado, "cancelado");
        if (c->term) {
            if (c->tarefa_n && !(c->turno_web && sucesso)) {
                GString *l = g_string_new(NULL);
                for (int i = 0; i < c->tarefa_n; i++) { tarefa_ui_linha(c, l, i, TRUE); if (i + 1 < c->tarefa_n) g_string_append_c(l, '\n'); }
                saida_linha(c, "%sPassos:%s", sgr(c, EST_FORTE), sgr0(c));
                saida_linha(c, "%s", l->str);
                g_string_free(l, TRUE);
            }
            if (cancelada)
                saida_linha(c, "%s  ✗ tarefa cancelada: todos os passos em andamento foram interrompidos e os pendentes não rodaram%s", sgr(c, EST_AVISO), sgr0(c));
            else if (sucesso && !c->turno_web)
                saida_linha(c, "%s  ✓ verificação: todos os critérios têm evidência — coleta concluída em %s%s", sgr(c, EST_OK), t, sgr0(c));
            else if (!c->turno_web)
                saida_linha(c, "%s  ✗ verificação: %s — evidências ausentes: %s (coleta: %s)%s", sgr(c, EST_ERRO),
                            veredito ? veredito : (estado ? estado : "?"), aus, t, sgr0(c));
        } else {
            saida_linha(c, "[resultado] estado=%s codigo_saida=%" G_GINT64_FORMAT " tarefa veredito=%s ausentes=%s tempo=%" G_GINT64_FORMAT "ms",
                        estado ? estado : "?", codigo, veredito ? veredito : "-", aus, exec_ms);
        }
        g_free(aus);
        g_free(apres);
        limpar_pendencias_de_execucao(c);
        tarefa_ui_limpar(c);
        if (c->turno_espera_resposta && !cancelada) {
            iniciar_progresso_pergunta(c);
            definir_fase(c, FASE_GERANDO_RESPOSTA);
        } else if (cancelada) {
            encerrar_turno(c, FALSE);
        } else {
            /* plano enviado sem pergunta (ex.: --json ou outro cliente): o
             * daemon ainda manda a 'resposta' com o relatório */
            c->turno_espera_resposta = TRUE;
            definir_fase(c, FASE_GERANDO_RESPOSTA);
        }
        return;
    }

    if (c->term) {
        const char *ferr = c->ferramenta_turno ? c->ferramenta_turno : (c->ferramenta_plano_pendente ? c->ferramenta_plano_pendente : "?");
        c->turno_web = sucesso && g_str_has_prefix(ferr, "web.");
        if (sucesso) {
            char t[32];
            ms_legivel(t, sizeof t, exec_ms < 0 ? 0 : exec_ms);
            saida_linha(c, "%s  ✓ dados reais recebidos de %s em %s%s", sgr(c, EST_OK), ferr, t, sgr0(c));
            if (apres && *apres) {
                saida_linha(c, "");
                saida_linha(c, "%s", apres);
                saida_linha(c, "");
            } else if (c->ultimo_dados_json) {
                char *d = sanear(c->ultimo_dados_json);
                saida_linha(c, "  dados: %s", d);
                g_free(d);
            }
        } else if (estado && !strcmp(estado, "cancelado")) {
            saida_linha(c, "%s  ✗ execução cancelada%s", sgr(c, EST_AVISO), sgr0(c));
        } else {
            saida_linha(c, "%s  ✗ execução de %s terminou com estado=%s (código %" G_GINT64_FORMAT ")%s",
                        sgr(c, EST_ERRO), ferr, estado ? estado : "?", codigo, sgr0(c));
        }
    } else {
        saida_linha(c, "[resultado] estado=%s codigo_saida=%" G_GINT64_FORMAT, estado ? estado : "?", codigo);
        if (c->ultimo_dados_json)
            saida_linha(c, "  dados: %s", c->ultimo_dados_json);
        if (apres && *apres)
            saida_linha(c, "%s", apres);
    }
    g_free(apres);
    gboolean cancelado = estado && !strcmp(estado, "cancelado");
    limpar_pendencias_de_execucao(c);
    if (c->turno_espera_resposta && !cancelado) {
        /* o daemon vai fechar o turno com 'resposta' (resumo fundamentado
         * do modelo, ou determinística se a execução falhou) */
        iniciar_progresso_pergunta(c);
        definir_fase(c, FASE_GERANDO_RESPOSTA);
    } else {
        encerrar_turno(c, sucesso);
    }
}

static void tratar_msg_erro(LiaChat *c, const LiaMsg *m)
{
    const char *codigo = campo_str(m->dados, "codigo");
    char *mensagem = sanear(campo_str(m->dados, "mensagem"));
    const char *detalhe = campo_str(m->dados, "detalhe");
    gboolean cancelado = codigo && !strcmp(codigo, "cancelado");
    if (c->term)
        saida_linha(c, "%s  ✗ %s%s: %s%s%s%s", sgr(c, cancelado ? EST_AVISO : EST_ERRO), cancelado ? "" : "erro ",
                    codigo ? codigo : "?", mensagem, detalhe ? " — " : "", detalhe ? detalhe : "", sgr0(c));
    else
        saida_linha(c, "[erro] %s: %s%s%s", codigo ? codigo : "?", mensagem,
                    detalhe ? " — " : "", detalhe ? detalhe : "");
    g_free(mensagem);
    if (codigo && !strcmp(codigo, "protocolo_incompativel")) {
        saida_linha(c, "[local] protocolo incompatível com o daemon — encerrando");
        c->codigo_saida = 3;
        c->deve_sair = TRUE;
        g_main_loop_quit(c->loop);
        return;
    }
    /* um erro de validação imediata para a última mensagem que enviamos
     * (em_resposta_a == nosso id) encerra a pendência correspondente —
     * tratar_solicitar/tratar_perguntar/tratar_aprovar/tratar_cancelar
     * sempre respondem assim para rejeições síncronas. A confirmação de um
     * cancelamento de pergunta/resumo ("cancelado") também chega assim. */
    if (m->em_resposta_a && c->ultimo_id_enviado && !strcmp(m->em_resposta_a, c->ultimo_id_enviado)) {
        gboolean recusa_de_cancelar = codigo && !strcmp(codigo, "estado_invalido") &&
                                      (c->pergunta_em_andamento || c->execucao_id_pendente || c->fase == FASE_GERANDO_RESPOSTA);
        if (recusa_de_cancelar)
            return; /* o daemon só recusou o nosso 'cancelar' (nada a cancelar do lado dele): o turno segue como estava */
        if (c->plano_id_pendente) limpar_pendencias_de_execucao(c);
        c->pergunta_em_andamento = FALSE;
        c->pergunta_abandonada_localmente = FALSE;
        parar_progresso_pergunta(c);
        encerrar_turno(c, FALSE);
        return;
    }
    /* erro assíncrono (resultado do modelo, falha/timeout de execução —
     * sessiond.c nunca usa em_resposta_a para esses): limpa as pendências
     * que dependem de uma única resposta assíncrona por conexão — nunca
     * deixa o estado local travado esperando algo que o daemon já resolveu
     * de outro jeito. Uma falha de EXECUÇÃO é seguida de 'resultado'
     * (estado=falha) e, no fluxo agente, de uma 'resposta' determinística:
     * o turno só fecha quando essas chegarem. */
    if (c->pergunta_em_andamento) {
        c->pergunta_em_andamento = FALSE;
        c->pergunta_abandonada_localmente = FALSE;
        parar_progresso_pergunta(c);
        encerrar_turno(c, FALSE);
    } else if (c->fase == FASE_GERANDO_RESPOSTA) {
        parar_progresso_pergunta(c);
        encerrar_turno(c, FALSE);
    }
}

static JsonObject *campo_obj(JsonObject *obj, const char *chave)
{
    if (!obj || !json_object_has_member(obj, chave)) return NULL;
    JsonNode *n = json_object_get_member(obj, chave);
    return JSON_NODE_TYPE(n) == JSON_NODE_OBJECT ? json_node_get_object(n) : NULL;
}

static void enviar_solicitar_da_proposta(LiaChat *c, const char *modo);

static gboolean plano_tem_web(JsonObject *plano)
{
    if (!plano || !json_object_has_member(plano, "passos")) return FALSE;
    JsonNode *n = json_object_get_member(plano, "passos");
    if (JSON_NODE_TYPE(n) != JSON_NODE_ARRAY) return FALSE;
    JsonArray *passos = json_node_get_array(n);
    for (guint i = 0; i < json_array_get_length(passos); i++) {
        JsonNode *p = json_array_get_element(passos, i);
        if (JSON_NODE_TYPE(p) != JSON_NODE_OBJECT) continue;
        const char *f = campo_str(json_node_get_object(p), "ferramenta");
        if (f && (!strcmp(f, "web.pesquisar") || !strcmp(f, "web.abrir"))) return TRUE;
    }
    return FALSE;
}

static void tratar_msg_proposta(LiaChat *c, const LiaMsg *m)
{
    const char *pergunta_id = campo_str(m->dados, "pergunta_id");
    if (!c->pergunta_em_andamento) {
        saida_linha(c, "[local] ignorado: proposta recebida sem nenhuma pergunta em andamento (talvez já cancelada)");
        return;
    }
    c->pergunta_em_andamento = FALSE;
    parar_progresso_pergunta(c);
    c->pergunta_abandonada_localmente = FALSE;

    char *texto = sanear(campo_str(m->dados, "resposta_texto"));
    g_strstrip(texto);
    JsonObject *intencao = campo_obj(m->dados, "intencao");
    JsonObject *validacao = campo_obj(m->dados, "validacao");
    const char *veredito = campo_str(validacao, "veredito");
    char *motivo = sanear(campo_str(validacao, "motivo"));
    const char *do_modelo = campo_str(validacao, "ferramenta_do_modelo");
    gboolean auto_ok = campo_bool(validacao, "execucao_automatica_permitida", FALSE);
    gboolean requer = campo_bool(validacao, "requer_aprovacao", FALSE);
    gboolean corrigida = veredito && !strcmp(veredito, "corrigida");
    c->t_proposta_ms = campo_int(m->dados, "tempo_modelo_ms", -1);

    const char *ferramenta = NULL;
    JsonObject *fp = campo_obj(m->dados, "ferramenta_proposta");
    if (fp) ferramenta = campo_str(fp, "ferramenta");
    const char *compat = campo_str(validacao, "compatibilidade");

    if (c->proposta_pendente) { proposta_pendente_liberar(c->proposta_pendente); c->proposta_pendente = NULL; }
    JsonObject *pp = campo_obj(m->dados, "plano_proposto");
    if (pp) {
        /* TAREFA: plano de vários passos já validado pelo daemon */
        PropostaPendente *p = g_new0(PropostaPendente, 1);
        p->pergunta_id = g_strdup(pergunta_id);
        p->ferramenta = g_strdup("(tarefa de vários passos)");
        p->parametros_json = g_strdup("{}");
        p->plano_id = g_strdup(campo_str(pp, "plano_id"));
        p->plano_hash = g_strdup(campo_str(pp, "hash"));
        c->proposta_pendente = p;
        tarefa_ui_limpar(c);
        tarefa_ui_carregar(c, pp);
        gboolean auto_tarefa = c->autopilot && auto_ok && !requer;
        gboolean web_interativa = c->autopilot && requer && plano_tem_web(pp);
        gboolean parcial = compat && !strcmp(compat, "parcial");
        if (c->term) {
            saida_linha(c, "%s  ✓ tarefa: %s (%d passos%s)%s", sgr(c, EST_FRACO), c->tarefa_objetivo ? c->tarefa_objetivo : "?", c->tarefa_n,
                        parcial ? "; compatibilidade PARCIAL: os dados coletados não respondem à pergunta de fundo" : "", sgr0(c));
            if (!auto_tarefa && !web_interativa)
                saida_linha(c, "%s  ⏸ a tarefa não roda sozinha (%s) — /plano para solicitar, ou faça outra pergunta%s", sgr(c, EST_AVISO),
                            !c->autopilot ? "modo manual" : (requer ? "exige aprovação" : "não é leitura pura"), sgr0(c));
            else if (web_interativa)
                saida_linha(c, "%s  ✓ pesquisa web pronta — solicitando agora; a aprovação aparecerá ao vivo%s", sgr(c, EST_FRACO), sgr0(c));
        } else {
            saida_linha(c, "[validação] tarefa=%s passos=%d compatibilidade=%s veredito=%s: %s", campo_str(pp, "intencao") ? campo_str(pp, "intencao") : "-",
                        c->tarefa_n, compat ? compat : "-", veredito ? veredito : "?", motivo);
            if (auto_tarefa)
                saida_linha(c, "[local] tarefa de %d passos selecionada — somente leitura, validada pelo daemon, executando automaticamente (LIA_CHAT_AUTOPILOT=read-only)", c->tarefa_n);
            else
                saida_linha(c, "[local] tarefa sugerida: %d passos — use /plano para solicitar", c->tarefa_n);
        }
        g_free(texto);
        g_free(motivo);
        if (auto_tarefa || web_interativa) enviar_solicitar_da_proposta(c, "executar");
        else { tarefa_ui_limpar(c); encerrar_turno(c, FALSE); }
        return;
    }
    if (ferramenta) {
        PropostaPendente *p = g_new0(PropostaPendente, 1);
        p->pergunta_id = g_strdup(pergunta_id);
        p->ferramenta = g_strdup(ferramenta);
        p->parametros_json = json_object_has_member(fp, "parametros")
            ? json_para_texto(json_object_get_member(fp, "parametros")) : g_strdup("{}");
        const char *just = campo_str(fp, "justificativa");
        p->justificativa = just ? g_strdup(just) : NULL;
        c->proposta_pendente = p;
    }
    /* Piloto automático: só segue sozinho quando o DAEMON marcou a
     * proposta como leitura pura E a política local é read-only. O que
     * segue é o mesmo 'solicitar' de /plano — o daemon valida de novo,
     * monta o plano e, se por qualquer motivo o plano exigir aprovação,
     * para e pergunta do mesmo jeito (tratar_msg_plano). */
    gboolean vai_auto = ferramenta && c->autopilot && auto_ok && !requer;
    if (!ferramenta && *texto) historico_adicionar(c, "Lia", texto);

    if (c->term) {
        if (intencao)
            saida_linha(c, "%s  ✓ intenção: %s (confiança %s)%s", sgr(c, EST_FRACO),
                        campo_str(intencao, "descricao") ? campo_str(intencao, "descricao") : "?",
                        campo_str(intencao, "confianca") ? campo_str(intencao, "confianca") : "?", sgr0(c));
        if (ferramenta) {
            saida_linha(c, "%s  ✓ ferramenta: %s %s%s", sgr(c, EST_FRACO), ferramenta, c->proposta_pendente->parametros_json, sgr0(c));
            if (corrigida && do_modelo && strcmp(do_modelo, ferramenta) != 0)
                saida_linha(c, "%s    corrigido pela validação independente: o modelo propôs %s%s", sgr(c, EST_AVISO), do_modelo, sgr0(c));
            else if (veredito && !strcmp(veredito, "rota_direta"))
                saida_linha(c, "%s    rota direta: intenção de alta confiança, modelo não consultado para escolher%s", sgr(c, EST_FRACO), sgr0(c));
            else if (corrigida)
                saida_linha(c, "%s    validação: %s%s", sgr(c, EST_FRACO), motivo, sgr0(c));
            if (!vai_auto) {
                if (*texto) saida_linha(c, "%sLia:%s %s", sgr(c, EST_FORTE), sgr0(c), texto);
                saida_linha(c, "%s  ⏸ %s não roda sozinha (%s) — /plano para solicitar, /simular, ou faça outra pergunta%s",
                            sgr(c, EST_AVISO), ferramenta,
                            !c->autopilot ? "modo manual" : (requer ? "exige aprovação" : "não é leitura pura"), sgr0(c));
            }
        } else {
            if (veredito && strcmp(veredito, "aceita") != 0)
                saida_linha(c, "%s  ✗ validação (%s%s%s): %s%s", sgr(c, EST_AVISO), veredito, compat ? "; ferramenta " : "", compat ? compat : "", motivo, sgr0(c));
            saida_linha(c, "%sLia:%s %s", sgr(c, EST_FORTE), sgr0(c), texto);
        }
    } else {
        if (*texto || !ferramenta)
            saida_linha(c, "[modelo] %s", texto);
        if (validacao && (intencao || (veredito && strcmp(veredito, "aceita") != 0)))
            saida_linha(c, "[validação] intenção=%s veredito=%s modelo_propôs=%s: %s",
                        intencao && campo_str(intencao, "id") ? campo_str(intencao, "id") : "-",
                        veredito ? veredito : "?", do_modelo ? do_modelo : "-", motivo);
        if (ferramenta && vai_auto)
            saida_linha(c, "[local] ferramenta selecionada: %s (parâmetros: %s) — leitura segura validada pelo daemon, executando automaticamente (LIA_CHAT_AUTOPILOT=read-only)",
                        ferramenta, c->proposta_pendente->parametros_json);
        else if (ferramenta)
            saida_linha(c, "[local] ferramenta sugerida: %s (parâmetros: %s) — use /plano para solicitar (executar) ou /simular",
                        ferramenta, c->proposta_pendente->parametros_json);
    }
    g_free(texto);
    g_free(motivo);

    if (vai_auto)
        enviar_solicitar_da_proposta(c, "executar");
    else
        encerrar_turno(c, ferramenta == NULL); /* só texto: turno concluído; proposta pendente: aguardando /plano */
}

static void tratar_msg_resposta(LiaChat *c, const LiaMsg *m)
{
    char *texto = sanear(campo_str(m->dados, "resposta_texto"));
    g_strstrip(texto);
    char *aviso = campo_str(m->dados, "aviso") ? sanear(campo_str(m->dados, "aviso")) : NULL;
    const char *origem = campo_str(m->dados, "origem");
    gboolean deterministica = origem && !strcmp(origem, "deterministica");
    gboolean resposta_web = c->turno_web ||
                            (c->ferramenta_turno && g_str_has_prefix(c->ferramenta_turno, "web.")) ||
                            (strstr(texto, "Evidências") && strstr(texto, "web.pesquisar"));
    c->t_resposta_ms = campo_int(m->dados, "tempo_modelo_ms", -1);
    parar_progresso_pergunta(c);
    gboolean extrativa = origem && !strcmp(origem, "modelo_extrativa");
    if (resposta_web) {
        /* A ferramenta já mostrou os resultados, resumos e links na ordem
         * recebida. A resposta genérica do verificador só repetiria o estado
         * da coleta; encerra o turno deixando o resultado como última saída. */
        g_free(texto);
        g_free(aviso);
        encerrar_turno(c, TRUE);
        return;
    }
    historico_adicionar(c, "Lia", texto);
    if (c->term && strstr(texto, "\nEvidências")) {
        /* relatório de tarefa: destaca o título de cada seção */
        static const char *const SECOES[] = { "RESULTADO ", "Motivo:", "Dados ausentes:", "Conclusão:", "Evidências", "Limitações:",
                                               "Ações realizadas", "Ações não realizadas:", "Próximo passo:", "Leitura do modelo", NULL };
        char **linhas = g_strsplit(texto, "\n", -1);
        GString *o = g_string_new(NULL);
        for (int i = 0; linhas[i]; i++) {
            const char *l = linhas[i];
            gboolean titulo = FALSE;
            for (int k = 0; SECOES[k] && !titulo; k++) {
                if (!g_str_has_prefix(l, SECOES[k])) continue;
                const char *dois = strchr(l, ':');
                gsize corte = dois ? (gsize)(dois - l) + 1 : strlen(l);
                g_string_append_printf(o, "%s%.*s%s%s", sgr(c, EST_FORTE), (int)corte, l, sgr0(c), l + corte);
                titulo = TRUE;
            }
            if (!titulo) g_string_append(o, l);
            if (linhas[i + 1]) g_string_append_c(o, '\n');
        }
        g_strfreev(linhas);
        g_free(texto);
        texto = g_string_free(o, FALSE);
    }
    if (c->term) {
        saida_linha(c, "%s%s:%s %s", sgr(c, EST_FORTE), resposta_web ? "Resposta" : "Lia", sgr0(c), texto);
        if (!resposta_web && origem && !strcmp(origem, "deterministica+modelo"))
            saida_linha(c, "%s  · relatório montado pelo daemon com os dados reais; a \"leitura do modelo\" ao final foi conferida contra eles%s", sgr(c, EST_FRACO), sgr0(c));
        else if (deterministica && !resposta_web)
            saida_linha(c, "%s  · resposta montada pelo daemon só com os dados reais acima (sem texto livre do modelo)%s", sgr(c, EST_FRACO), sgr0(c));
        else if (extrativa && !resposta_web)
            saida_linha(c, "%s  · o modelo escolheu quais linhas respondem; o texto foi copiado dos dados reais%s", sgr(c, EST_FRACO), sgr0(c));
        else if (!resposta_web)
            saida_linha(c, "%s  · resumo do modelo, conferido contra os dados reais%s", sgr(c, EST_FRACO), sgr0(c));
        if (aviso && !resposta_web) saida_linha(c, "%s  ! %s%s", sgr(c, EST_AVISO), aviso, sgr0(c));
    } else {
        saida_linha(c, deterministica ? "[resposta, determinística] %s" : (extrativa ? "[resposta, extrativa] %s" :
                    (origem && !strcmp(origem, "deterministica+modelo")) ? "[resposta, determinística+modelo] %s" : "[modelo, resumo] %s"), texto);
        if (aviso) saida_linha(c, "[aviso] %s", aviso);
    }
    g_free(texto);
    g_free(aviso);
    encerrar_turno(c, TRUE);
}

static void processar_msg_recebida(LiaChat *c, const LiaMsg *m)
{
    LiaSessaoErro erro = lia_sessao_receber(&c->sessao, m);
    if (erro != LIA_SESSAO_OK)
        saida_linha(c, "[local] aviso: mensagem fora de ordem detectada localmente (código %d)", (int)erro);

    switch (m->tipo) {
    case LIA_MSG_PLANO: tratar_msg_plano(c, m); break;
    case LIA_MSG_PROGRESSO: tratar_msg_progresso_ou_saida(c, m, TRUE); break;
    case LIA_MSG_SAIDA: tratar_msg_progresso_ou_saida(c, m, FALSE); break;
    case LIA_MSG_RESULTADO: tratar_msg_resultado(c, m); break;
    case LIA_MSG_AUDITORIA:
        /* numa tarefa há um registro por passo: a linha do próprio passo já basta */
        if (c->term) { if (!c->tarefa_ativa) saida_linha(c, "%s  · auditoria registrada%s", sgr(c, EST_FRACO), sgr0(c)); }
        else saida_linha(c, "[auditoria registrada]");
        break;
    case LIA_MSG_ERRO: tratar_msg_erro(c, m); break;
    case LIA_MSG_PROPOSTA: tratar_msg_proposta(c, m); break;
    case LIA_MSG_RESPOSTA: tratar_msg_resposta(c, m); break;
    default: saida_linha(c, "[local] mensagem de tipo inesperado do daemon ignorada"); break;
    }
}

/* --------------------------------------------------------- comandos --- */

static void processar_comando(LiaChat *c, const char *linha_bruta);

static void finalizar(LiaChat *c)
{
    c->deve_sair = TRUE;
    if (c->loop) g_main_loop_quit(c->loop);
}

static gboolean afirmativo(const char *resposta)
{
    char *r = g_ascii_strdown(resposta, -1);
    g_strstrip(r);
    gboolean sim = !strcmp(r, "s") || !strcmp(r, "sim") || !strcmp(r, "y") || !strcmp(r, "yes");
    g_free(r);
    return sim;
}

static void processar_comando(LiaChat *c, const char *linha_bruta)
{
    char *linha = g_strdup(linha_bruta);
    g_strstrip(linha);

    if (!*linha) { g_free(linha); return; }

    if (c->aguardando_decisao_plano && linha[0] != '/') {
        tratar_decisao_plano(c, afirmativo(linha));
        g_free(linha);
        return;
    }

    if (linha[0] != '/') {
        enviar_perguntar(c, linha);
        g_free(linha);
        return;
    }

    char **partes = g_strsplit(linha, " ", 3);
    const char *cmd = partes[0];

    if (!strcmp(cmd, "/help") || !strcmp(cmd, "/ajuda")) {
        imprimir_ajuda(c);
    } else if (!strcmp(cmd, "/status")) {
        imprimir_status(c);
    } else if (!strcmp(cmd, "/modelo")) {
        probe_modelo(c, TRUE);
    } else if (!strcmp(cmd, "/backend")) {
        if (!partes[1] || (strcmp(partes[1], "mock") && strcmp(partes[1], "ollama") && strcmp(partes[1], "llama_cpp"))) {
            saida_linha(c, "[local] uso: /backend mock|ollama|llama_cpp");
        } else {
            g_free(c->backend_assumido);
            c->backend_assumido = g_strdup(partes[1]);
            saida_linha(c, "[local] backend assumido agora: %s (não controla o daemon remotamente — ele decide o seu próprio no início, LIA_SESSIOND_MODELO)", c->backend_assumido);
        }
    } else if (!strcmp(cmd, "/arquivos")) {
        if (partes[1] && !strcmp(partes[1], "listar")) arquivos_listar(c);
        else saida_linha(c, "[local] uso: /arquivos listar");
    } else if (!strcmp(cmd, "/arquivo")) {
        if (partes[1] && !strcmp(partes[1], "adicionar") && partes[2]) arquivo_adicionar(c, partes[2]);
        else if (partes[1] && !strcmp(partes[1], "remover") && partes[2]) arquivo_remover(c, partes[2]);
        else saida_linha(c, "[local] uso: /arquivo adicionar CAMINHO | /arquivo remover INDICE");
    } else if (!strcmp(cmd, "/plano")) {
        if (c->plano_id_pendente) imprimir_plano(c, c->plano_dados_pendente);
        else if (c->proposta_pendente) enviar_solicitar_da_proposta(c, "executar");
        else saida_linha(c, "[local] nada pendente");
    } else if (!strcmp(cmd, "/simular")) {
        if (c->plano_id_pendente) saida_linha(c, "[local] já há um plano apresentado — aprove ou rejeite, o modo não muda agora");
        else enviar_solicitar_da_proposta(c, "simular");
    } else if (!strcmp(cmd, "/aprovar")) {
        tratar_decisao_plano(c, TRUE);
    } else if (!strcmp(cmd, "/rejeitar")) {
        tratar_decisao_plano(c, FALSE);
    } else if (!strcmp(cmd, "/cancelar")) {
        tratar_cancelar(c);
    } else if (!strcmp(cmd, "/limpar")) {
        if (c->term) term_limpar_tela(c->term);
    } else if (!strcmp(cmd, "/dados")) {
        if (c->ultimo_dados_json) {
            char *d = sanear(c->ultimo_dados_json);
            saida_linha(c, "%s", d);
            g_free(d);
        } else {
            saida_linha(c, "[local] nenhum resultado recebido ainda nesta sessão");
        }
    } else if (!strcmp(cmd, "/autopilot")) {
        if (partes[1] && !strcmp(partes[1], "read-only")) c->autopilot = TRUE;
        else if (partes[1] && !strcmp(partes[1], "manual")) c->autopilot = FALSE;
        else if (partes[1]) { saida_linha(c, "[local] uso: /autopilot read-only|manual (nenhum outro valor existe)"); g_strfreev(partes); g_free(linha); return; }
        saida_linha(c, "[local] piloto automático: %s", c->autopilot
            ? "read-only — leituras que o daemon validar como seguras executam sozinhas; escrita/admin/hardware continuam exigindo aprovação"
            : "manual — toda ferramenta pede /plano");
        if (c->fase == FASE_OCIOSO) definir_fase(c, FASE_OCIOSO);
    } else if (!strcmp(cmd, "/sair") || !strcmp(cmd, "/exit") || !strcmp(cmd, "/quit")) {
        finalizar(c);
    } else {
        saida_linha(c, "[local] comando desconhecido: %s (digite /help)", cmd);
    }
    g_strfreev(partes);
    g_free(linha);
}

/* -------------------------------------------------- modo --json cru --- */

static void processar_linha_json(LiaChat *c, const char *linha)
{
    LiaMsg m;
    LiaMsgErro erro = lia_msg_parsear(linha, &m);
    if (erro != LIA_MSG_OK) {
        fprintf(stderr, "lia-chat: linha rejeitada localmente (%s)\n", lia_msg_erro_texto(erro));
        return;
    }
    LiaEnvioErro erro_envio = lia_sessao_validar_envio(&c->sessao, m.tipo, NULL, 0);
    if (erro_envio != LIA_ENVIO_OK) {
        fprintf(stderr, "lia-chat: envio recusado localmente (código %d)\n", (int)erro_envio);
        lia_msg_liberar(&m);
        return;
    }
    if (c->conexao && c->saida) {
        char *com_quebra = g_strdup_printf("%s\n", linha);
        g_output_stream_write_all(c->saida, com_quebra, strlen(com_quebra), NULL, NULL, NULL);
        g_free(com_quebra);
    }
    if (m.tipo == LIA_MSG_PERGUNTAR)
        c->pergunta_em_andamento = TRUE;
    lia_msg_liberar(&m);
}

/* --------------------------------------------------------- callbacks --- */

static void agendar_leitura_socket(LiaChat *c);

/* Uma linha completa chegou do terminal interativo (chat/terminal.c já a
 * ecoou no histórico e já redesenhou a entrada vazia). NULL = Ctrl-D. */
static void ao_linha_terminal(const char *linha, gpointer dados)
{
    LiaChat *c = dados;
    if (!linha) {
        finalizar(c);
        return;
    }
    processar_comando(c, linha);
    flush_saida(c);
}

static gboolean ao_stdin_pronto(gint fd, GIOCondition condicao, gpointer dados)
{
    (void)fd;
    LiaChat *c = dados;
    gboolean leu = term_entrada_disponivel(c->term);
    if (!leu && (condicao & (G_IO_HUP | G_IO_ERR | G_IO_IN))) {
        /* fd pronto, nada para ler: o terminal fechou */
        finalizar(c);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean ao_sigwinch(gpointer dados)
{
    LiaChat *c = dados;
    if (c->term) term_redimensionado(c->term);
    return G_SOURCE_CONTINUE;
}

/* Modo não interativo (stdin redirecionado/pipe — scripts, testes de
 * integração) e modo --json: sem região viva, sem edição própria — leitor
 * de linha simples, uma linha de texto por linha de saída. */
static void agendar_leitura_stdin_pipe(LiaChat *c);

static void ao_ler_stdin_pipe(GObject *origem, GAsyncResult *res, gpointer dados)
{
    LiaChat *c = dados;
    GError *erro = NULL;
    gsize tam = 0;
    char *linha = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(origem), res, &tam, &erro);
    if (erro) { fprintf(stderr, "lia-chat: erro lendo stdin: %s\n", erro->message); g_error_free(erro); finalizar(c); return; }
    if (!linha) { finalizar(c); return; } /* EOF */

    if (c->modo_json) processar_linha_json(c, linha);
    else processar_comando(c, linha);
    g_free(linha);
    flush_saida(c);

    if (!c->deve_sair) agendar_leitura_stdin_pipe(c);
}

static void agendar_leitura_stdin_pipe(LiaChat *c)
{
    g_data_input_stream_read_line_async(c->entrada_stdin, G_PRIORITY_DEFAULT, NULL, ao_ler_stdin_pipe, c);
}

static void ao_ler_socket(GObject *origem, GAsyncResult *res, gpointer dados)
{
    LiaChat *c = dados;
    GError *erro = NULL;
    gsize tam = 0;
    char *linha = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(origem), res, &tam, &erro);
    if (erro) {
        if (c->term) { saida_linha(c, "%s  ✗ erro lendo o daemon: %s%s", sgr(c, EST_ERRO), erro->message, sgr0(c)); flush_saida(c); }
        else fprintf(stderr, "lia-chat: erro lendo o daemon: %s\n", erro->message);
        g_error_free(erro);
        if (!c->deve_sair) c->codigo_saida = 1;
        finalizar(c);
        return;
    }
    if (!linha) {
        if (c->term) { saida_linha(c, "%s  ✗ o daemon desconectou — encerrando%s", sgr(c, EST_ERRO), sgr0(c)); flush_saida(c); }
        else fprintf(stderr, "lia-chat: daemon desconectou\n");
        if (!c->deve_sair) c->codigo_saida = 1;
        finalizar(c);
        return;
    }

    if (c->modo_json) {
        printf("%s\n", linha);
        fflush(stdout);
    } else {
        LiaMsg m;
        LiaMsgErro merro = lia_msg_parsear(linha, &m);
        if (merro != LIA_MSG_OK) {
            saida_linha(c, "[local] mensagem do daemon rejeitada localmente (%s)", lia_msg_erro_texto(merro));
        } else {
            processar_msg_recebida(c, &m);
            lia_msg_liberar(&m);
        }
        flush_saida(c);
    }
    g_free(linha);
    agendar_leitura_socket(c);
}

static void agendar_leitura_socket(LiaChat *c)
{
    g_data_input_stream_read_line_async(c->entrada_socket, G_PRIORITY_DEFAULT, NULL, ao_ler_socket, c);
}

/* Mensagem de aviso do próprio Ctrl-C: sempre mostrada (mesmo em --json,
 * onde saida_linha() é um no-op de propósito), por isso não reusa
 * saida_linha — mas segue a mesma regra de nunca estragar a linha de
 * edição ativa quando há uma. */
static void anunciar_sigint(LiaChat *c, const char *texto)
{
    /* só acumula — flush_saida() é chamada uma vez só, ao final de
     * ao_sigint, pelo mesmo motivo de saida_linha() (ver comentário lá). */
    g_string_append(c->buffer_saida, texto);
    g_string_append_c(c->buffer_saida, '\n');
}

static gboolean ao_sigint(gpointer dados)
{
    LiaChat *c = dados;
    gint64 agora = g_get_monotonic_time();
    if (c->ultimo_sigint_us && (agora - c->ultimo_sigint_us) < 2 * G_USEC_PER_SEC) {
        anunciar_sigint(c, "lia-chat: Ctrl-C duas vezes — encerrando");
        c->codigo_saida = 130;
        finalizar(c);
        flush_saida(c);
        return G_SOURCE_CONTINUE;
    }
    c->ultimo_sigint_us = agora;
    if ((c->plano_id_pendente && c->aguardando_decisao_plano) || c->execucao_id_pendente ||
        c->pergunta_em_andamento || c->fase == FASE_GERANDO_RESPOSTA) {
        tratar_cancelar(c);
    } else {
        anunciar_sigint(c, "lia-chat: nada para cancelar; Ctrl-C de novo em até 2s para encerrar, ou /sair");
    }
    flush_saida(c);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------- main --- */

static void imprimir_uso(const char *argv0)
{
    fprintf(stderr,
        "uso: %s [--socket CAMINHO] [--backend mock|ollama|llama_cpp] [--modelo NOME] [--json] [--help]\n"
        "\n"
        "Ambiente:\n"
        "  LIA_CHAT_AUTOPILOT=read-only  (padrão) leituras que o daemon validar como seguras executam\n"
        "                                sozinhas; escrita/admin/hardware sempre pedem aprovação\n"
        "  LIA_CHAT_AUTOPILOT=manual     toda ferramenta pede confirmação (/plano)\n"
        "  (qualquer outro valor: recusa iniciar, código 2)\n"
        "\n"
        "Códigos de saída:\n"
        "  0   saída normal (/sair ou fim de entrada)\n"
        "  1   não foi possível conectar ao daemon, ou o daemon desconectou\n"
        "  2   uso inválido (argumento de linha de comando)\n"
        "  3   erro fatal de protocolo (ex.: protocolo_incompativel)\n"
        "  130 interrompido (Ctrl-C duas vezes em menos de 2s)\n",
        argv0);
}

int main(int argc, char **argv)
{
    /* Sem isto, o processo nasce na locale "C" (padrão da libc,
     * independente do que o ambiente/terminal realmente usa) — e nessa
     * locale, libedit trata cada BYTE de um caractere UTF-8 multibyte
     * (qualquer acentuação do português: á, ã, ç, é, õ...) como uma
     * tecla solta em vez de reconhecer a sequência, descartando ou
     * corrompendo o caractere digitado (confirmado: "ação" digitado
     * chegava como "ao" — 'ç' e 'ã' simplesmente desapareciam). Isto
     * segue a variável de ambiente (LANG/LC_*) do terminal real da
     * pessoa; nunca força um valor fixo. */
    setlocale(LC_ALL, "");

    /* stdout é totalmente bufferizado por padrão quando não é um TTY
     * (redirecionado a arquivo/pipe) — sem isto, nada apareceria até o
     * processo terminar, mesmo em modo interativo real com stdout
     * redirecionado (ex.: dentro de outra ferramenta). Cada linha sai na
     * hora, sempre. */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

    LiaChat c = { 0 };
    c.codigo_saida = 0;

    /* Nenhuma alocação de heap antes do laço de argv abaixo, de propósito:
     * cada "uso inválido" dentro dele retorna direto (2, ou 0 para
     * --help), sem passar pela liberação do final de main() — alocar
     * antes (como este código fazia) vazava essas poucas alocações
     * nesses caminhos (achado rodando com AddressSanitizer/
     * LeakSanitizer: o próprio código de saída do processo vinha trocado
     * de 2 para 1, já que o LeakSanitizer força isso ao detectar o
     * vazamento). c.backend_assumido fica NULL durante o laço — a
     * branch de --backend já trata isso bem (g_free(NULL) é sempre
     * seguro); o padrão "mock" só é aplicado depois, se ninguém pediu
     * outro. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            imprimir_uso(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            c.socket_caminho = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "mock") && strcmp(argv[i], "ollama") && strcmp(argv[i], "llama_cpp")) {
                fprintf(stderr, "lia-chat: --backend precisa ser 'mock', 'ollama' ou 'llama_cpp'\n");
                return 2;
            }
            g_free(c.backend_assumido);
            c.backend_assumido = g_strdup(argv[i]);
        } else if (!strcmp(argv[i], "--modelo") && i + 1 < argc) {
            c.modelo_assumido = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--json")) {
            c.modo_json = TRUE;
        } else {
            fprintf(stderr, "lia-chat: opção desconhecida: %s\n", argv[i]);
            imprimir_uso(argv[0]);
            return 2;
        }
    }

    /* Política de piloto automático: só dois valores existem. Um valor
     * desconhecido NÃO cai em nenhum dos dois por palpite — recusa iniciar
     * (falha segura). Não existe valor que libere escrita/admin/hardware:
     * isso não é configurável por ambiente. */
    const char *ap = g_getenv("LIA_CHAT_AUTOPILOT");
    if (!ap || !*ap || !strcmp(ap, "read-only")) {
        c.autopilot = TRUE;
    } else if (!strcmp(ap, "manual")) {
        c.autopilot = FALSE;
    } else {
        fprintf(stderr, "lia-chat: LIA_CHAT_AUTOPILOT='%s' não é um valor conhecido — use 'read-only' ou 'manual'. "
                        "Recusando iniciar (falha segura; nenhum valor libera escrita, administração ou hardware).\n", ap);
        g_free(c.socket_caminho);
        g_free(c.backend_assumido);
        g_free(c.modelo_assumido);
        return 2;
    }
    c.t_proposta_ms = c.t_execucao_ms = c.t_resposta_ms = -1;

    if (!c.socket_caminho) {
        const char *home = g_get_home_dir();
        c.socket_caminho = g_strdup_printf("%s/.lia/run/session.sock", home);
    }

    c.interativo = isatty(STDIN_FILENO) ? TRUE : FALSE;

    GSocketAddress *endereco = g_unix_socket_address_new(c.socket_caminho);
    GSocketClient *cliente = g_socket_client_new();
    g_socket_client_set_timeout(cliente, 5);
    GError *erro = NULL;
    c.conexao = g_socket_client_connect(cliente, G_SOCKET_CONNECTABLE(endereco), NULL, &erro);
    g_object_unref(endereco);
    g_object_unref(cliente);

    if (!c.conexao) {
        fprintf(stderr, "lia-chat: não foi possível conectar a %s (%s) — o daemon está rodando? (build/lia-sessiond)\n",
                c.socket_caminho, erro ? erro->message : "?");
        if (erro) g_error_free(erro);
        g_free(c.socket_caminho);
        g_free(c.backend_assumido);
        g_free(c.modelo_assumido);
        return 1;
    }
    /* o timeout do GSocketClient (acima) vale só para a tentativa de
     * conectar — GSocketClient propaga esse valor para o GSocket
     * resultante, e sem isto TODA leitura ociosa subsequente (esperar a
     * próxima mensagem do daemon, o normal entre uma pergunta e outra)
     * falharia com G_IO_ERROR_TIMED_OUT depois de 5s. Uma conversa não
     * tem prazo — zera para "sem timeout" assim que a conexão existe. */
    g_socket_set_timeout(g_socket_connection_get_socket(c.conexao), 0);

    if (!c.backend_assumido)
        c.backend_assumido = g_strdup("mock");
    c.arquivos = g_ptr_array_new_with_free_func((GDestroyNotify)arquivo_anexo_liberar);
    c.buffer_saida = g_string_new(NULL);
    c.historico_conversa = g_string_new(NULL);

    lia_sessao_iniciar(&c.sessao);
    c.sessao.modo_leitura = FALSE; /* lia-chat PODE enviar 'aprovar' — é o próprio ponto desta CLI */
    lia_sessao_conectar(&c.sessao);

    c.saida = g_io_stream_get_output_stream(G_IO_STREAM(c.conexao));
    GInputStream *fluxo_socket = g_io_stream_get_input_stream(G_IO_STREAM(c.conexao));
    c.entrada_socket = g_data_input_stream_new(fluxo_socket);

    c.loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, ao_sigint, &c); /* antes de qualquer saída: nunca há janela sem handler */

    if (!c.modo_json) {
        saida_linha(&c, "lia-chat: conectado a %s", c.socket_caminho);
        saida_linha(&c, "digite /help para ver os comandos");
        saida_linha(&c, "piloto automático: %s", c.autopilot
            ? "read-only — leituras seguras executam sozinhas; escrita/admin/hardware pedem aprovação (LIA_CHAT_AUTOPILOT=manual desliga)"
            : "manual — toda ferramenta pede /plano");
        flush_saida(&c); /* a região viva ainda não existe aqui — sempre texto simples */
    }

    if (c.interativo && !c.modo_json)
        c.term = term_novo(STDIN_FILENO, STDOUT_FILENO, "lia> ", ao_linha_terminal, &c);
    if (c.term) {
        /* Terminal interativo: chat/terminal.c assume o desenho (região
         * viva: status + separador + entrada) e a edição de linha. O
         * SIGINT já tem dono (g_unix_signal_add acima; o modo cru mantém
         * ISIG) e o SIGWINCH só pede um redesenho. */
        definir_fase(&c, FASE_OCIOSO);
        g_unix_fd_add(STDIN_FILENO, G_IO_IN | G_IO_HUP | G_IO_ERR, ao_stdin_pronto, &c);
        g_unix_signal_add(SIGWINCH, ao_sigwinch, &c);
    } else {
        /* stdin não é um terminal (pipe/arquivo/script, make teste-chat),
         * ou modo --json: leitor de linha simples, sem região viva. */
        GInputStream *fluxo_stdin = g_unix_input_stream_new(STDIN_FILENO, FALSE);
        c.entrada_stdin = g_data_input_stream_new(fluxo_stdin);
        g_object_unref(fluxo_stdin);
        agendar_leitura_stdin_pipe(&c);
    }

    agendar_leitura_socket(&c);

    g_main_loop_run(c.loop);

    if (c.term) { term_liberar(c.term); c.term = NULL; }
    if (c.entrada_stdin) g_object_unref(c.entrada_stdin);
    if (c.conexao) { g_io_stream_close(G_IO_STREAM(c.conexao), NULL, NULL); g_object_unref(c.conexao); }
    g_object_unref(c.entrada_socket);
    g_main_loop_unref(c.loop);
    lia_sessao_liberar(&c.sessao);
    g_ptr_array_free(c.arquivos, TRUE);
    g_string_free(c.buffer_saida, TRUE);
    g_string_free(c.historico_conversa, TRUE);
    if (c.proposta_pendente) proposta_pendente_liberar(c.proposta_pendente);
    if (c.plano_dados_pendente) json_object_unref(c.plano_dados_pendente);
    g_free(c.plano_id_pendente);
    g_free(c.ferramenta_plano_pendente);
    g_free(c.execucao_id_pendente);
    g_free(c.ultimo_id_enviado);
    g_free(c.ultimo_dados_json);
    g_free(c.plano_hash_pendente);
    g_free(c.tarefa_objetivo);
    g_free(c.tarefa_fase);
    for (int i = 0; i < TAREFA_MAX_PASSOS_UI; i++) { g_free(c.tarefa_ferramenta[i]); g_free(c.tarefa_estado[i]); }
    g_free(c.ferramenta_turno);
    g_free(c.socket_caminho);
    g_free(c.backend_assumido);
    g_free(c.modelo_assumido);

    return c.codigo_saida;
}
