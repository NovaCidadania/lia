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
 * Edição de linha no terminal (setas, histórico, Ctrl-A/E/K/W, etc.) via
 * libedit (BSD, API compatível com GNU readline) — nunca GNU readline em
 * si: a licença do próprio projeto Lia ainda está em aberto (PLANO.md
 * §8), e readline é GPL (contaminaria a licença do binário inteiro se
 * distribuído); libedit é BSD, sem essa amarra. Histórico só em memória
 * desta sessão — nunca gravado em disco por padrão (uma linha de chat
 * pode conter o que quer que a pessoa tenha digitado; persistir isso
 * sem política own seria o mesmo erro que o projeto já evita para
 * ~/.lia/humano/memoria — origem/confiança/versão explícitos antes de
 * guardar qualquer coisa).
 */
#define _DEFAULT_SOURCE
#include "../gui/protocolo.h"
#include "../sessiond/modelo.h"
#include "../sessiond/prompt.h"
#include <editline/readline.h>
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
} PropostaPendente;

static void proposta_pendente_liberar(PropostaPendente *p)
{
    if (!p) return;
    g_free(p->pergunta_id);
    g_free(p->ferramenta);
    g_free(p->parametros_json);
    g_free(p->justificativa);
    g_free(p);
}

typedef struct {
    LiaSessao sessao;
    GSocketConnection *conexao;
    GOutputStream *saida;
    GDataInputStream *entrada_socket;
    GDataInputStream *entrada_stdin; /* só usado quando !interativo (ver main()) */
    gboolean readline_ativo; /* true depois de rl_callback_handler_install (edição de linha real) */
    GString *buffer_saida;  /* ver flush_saida(): nunca chama o salva/redesenha do libedit mais de uma vez por "turno" */
    GMainLoop *loop;

    char *socket_caminho;
    char *backend_assumido;  /* "mock" ou "ollama" — só exibição/probe local, nunca controla o daemon remotamente */
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
} LiaChat;

/* --------------------------------------------------------- utilidades --- */

/* Escreve 'texto' (sem \n final) por cima da linha de edição ativa do
 * libedit, sem perder o que a pessoa já tinha digitado — o padrão
 * documentado para saída assíncrona durante uma leitura em modo
 * callback (mesma receita do manual do GNU Readline, que libedit
 * também segue): esconde a linha atual, imprime, redesenha a linha e o
 * prompt exatamente como estavam. Só chamada quando readline_ativo. */
static void imprimir_sobre_readline(const char *texto)
{
    /* NUNCA rl_save_prompt()/rl_restore_prompt()/rl_replace_line(): nesta
     * libedit, a partir da SEGUNDA chamada na mesma sessão (mesmo sem
     * nenhuma edição real em andamento) elas corrompem o estado interno
     * — setas param de recuperar histórico simples e caem em busca
     * incremental ("bck:"), confirmado isolando o problema num programa
     * mínimo até sobrar só essas três chamadas como causa. O que a
     * pessoa já digitou continua intacto o tempo todo no próprio
     * rl_line_buffer/rl_point — não precisa (e não deve) ser salvo e
     * restaurado à mão: só ir para uma linha nova, imprimir, e pedir
     * pro libedit redesenhar o que ele já sabe. */
    rl_crlf();
    fputs(texto, stdout);
    fputc('\n', stdout);
    rl_on_new_line();
    rl_forced_update_display();
}

/* saida_linha só ACUMULA — nunca chama imprimir_sobre_readline()
 * diretamente. Chamar o salva-prompt/redesenha do libedit várias vezes
 * em sequência rápida (uma por linha de uma resposta de várias linhas,
 * como /status) corrompe o estado interno dele e quebra o
 * reconhecimento de sequência de escape nas teclas seguintes (setas
 * param de recuperar o histórico simples e entram em busca incremental,
 * com um prompt "bck:" — comportamento confirmado isolando o problema
 * num programa mínimo). flush_saida(), chamada uma vez por "turno"
 * (depois de uma linha completa processada, ou de uma mensagem do
 * daemon), é o único lugar que de fato escreve na tela. */
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
    if (c->readline_ativo) {
        imprimir_sobre_readline(c->buffer_saida->str);
    } else {
        fputs(c->buffer_saida->str, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    g_string_truncate(c->buffer_saida, 0);
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

static void enviar_perguntar(LiaChat *c, const char *mensagem)
{
    if (c->pergunta_em_andamento && !c->pergunta_abandonada_localmente) {
        saida_linha(c, "[local] aguardando resposta da pergunta anterior (ou use /cancelar para abandoná-la)");
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
        c->pergunta_em_andamento = TRUE;
        c->pergunta_abandonada_localmente = FALSE;
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
    if (c->pergunta_em_andamento && !c->pergunta_abandonada_localmente) {
        /* o protocolo (docs/protocolo.md §9) não tem, nesta etapa, uma
         * forma de cancelar uma 'pergunta' em andamento no daemon — só
         * marcamos localmente para ignorar a resposta quando chegar. */
        c->pergunta_abandonada_localmente = TRUE;
        saida_linha(c, "[local] não há como cancelar uma pergunta ao modelo em andamento nesta etapa do protocolo — a resposta, quando chegar, será ignorada");
        return;
    }
    saida_linha(c, "[local] nada para cancelar");
}

/* ------------------------------------------------------- probe local --- */

static void probe_modelo(LiaChat *c, gboolean completo)
{
    if (c->modelo_assumido)
        g_setenv("LIA_OLLAMA_MODELO", c->modelo_assumido, TRUE);
    const ModeloBackend *m = !strcmp(c->backend_assumido, "ollama") ? modelo_ollama_obter() : modelo_mock_obter();
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
        if (c->interativo && !c->modo_json) { fputs("Aprovar este plano? [s/N] ", stdout); fflush(stdout); }
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
        "  /limpar                    limpa a tela\n"
        "  /sair                      encerra (ou Ctrl-D)\n"
        "\n"
        "Qualquer outra linha é enviada ao modelo como uma pergunta — exceto quando um\n"
        "plano está pendente: nesse caso a linha é lida como [s/N] (padrão: rejeitar).");
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
    imprimir_plano(c, m->dados);
    if (!c->aguardando_decisao_plano)
        saida_linha(c, "[local] sem aprovação necessária — executando");
}

static void tratar_msg_progresso_ou_saida(LiaChat *c, const LiaMsg *m, gboolean progresso)
{
    const char *exec_id = campo_str(m->dados, "execucao_id");
    if (exec_id && !c->execucao_id_pendente)
        c->execucao_id_pendente = g_strdup(exec_id);
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
    saida_linha(c, "[resultado] estado=%s codigo_saida=%" G_GINT64_FORMAT, estado ? estado : "?", codigo);
    if (json_object_has_member(m->dados, "dados")) {
        char *dados_texto = json_para_texto(json_object_get_member(m->dados, "dados"));
        saida_linha(c, "  dados: %s", dados_texto);
        g_free(dados_texto);
    }
    limpar_pendencias_de_execucao(c);
}

static void tratar_msg_erro(LiaChat *c, const LiaMsg *m)
{
    const char *codigo = campo_str(m->dados, "codigo");
    const char *mensagem = campo_str(m->dados, "mensagem");
    const char *detalhe = campo_str(m->dados, "detalhe");
    saida_linha(c, "[erro] %s: %s%s%s", codigo ? codigo : "?", mensagem ? mensagem : "",
                detalhe ? " — " : "", detalhe ? detalhe : "");
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
     * sempre respondem assim para rejeições síncronas. */
    if (m->em_resposta_a && c->ultimo_id_enviado && !strcmp(m->em_resposta_a, c->ultimo_id_enviado)) {
        if (c->plano_id_pendente) limpar_pendencias_de_execucao(c);
        c->pergunta_em_andamento = FALSE;
        c->pergunta_abandonada_localmente = FALSE;
        return;
    }
    /* erro assíncrono (resultado do modelo, timeout de execução —
     * sessiond.c nunca usa em_resposta_a para esses, ver
     * ao_concluir_ollama/ao_timeout_execucao): não há como saber ao
     * certo qual pendência terminou, então limpa as que dependem de uma
     * única resposta assíncrona por conexão — nunca deixa o estado local
     * travado esperando algo que o daemon já resolveu de outro jeito. */
    if (c->pergunta_em_andamento) {
        c->pergunta_em_andamento = FALSE;
        c->pergunta_abandonada_localmente = FALSE;
    }
    if (c->execucao_id_pendente)
        limpar_pendencias_de_execucao(c);
}

static void tratar_msg_proposta(LiaChat *c, const LiaMsg *m)
{
    const char *pergunta_id = campo_str(m->dados, "pergunta_id");
    if (!c->pergunta_em_andamento) {
        saida_linha(c, "[local] ignorado: proposta recebida sem nenhuma pergunta em andamento (talvez já cancelada)");
        return;
    }
    gboolean abandonada = c->pergunta_abandonada_localmente;
    c->pergunta_em_andamento = FALSE;
    c->pergunta_abandonada_localmente = FALSE;
    if (abandonada) {
        saida_linha(c, "[local] ignorado: resposta de uma pergunta cancelada localmente");
        return;
    }
    const char *texto = campo_str(m->dados, "resposta_texto");
    saida_linha(c, "[modelo] %s", texto ? texto : "");

    if (c->proposta_pendente) { proposta_pendente_liberar(c->proposta_pendente); c->proposta_pendente = NULL; }
    if (json_object_has_member(m->dados, "ferramenta_proposta")) {
        JsonNode *n = json_object_get_member(m->dados, "ferramenta_proposta");
        if (JSON_NODE_TYPE(n) == JSON_NODE_OBJECT) {
            JsonObject *fp = json_node_get_object(n);
            const char *ferramenta = campo_str(fp, "ferramenta");
            if (ferramenta) {
                PropostaPendente *p = g_new0(PropostaPendente, 1);
                p->pergunta_id = g_strdup(pergunta_id);
                p->ferramenta = g_strdup(ferramenta);
                p->parametros_json = json_object_has_member(fp, "parametros")
                    ? json_para_texto(json_object_get_member(fp, "parametros")) : g_strdup("{}");
                const char *just = campo_str(fp, "justificativa");
                p->justificativa = just ? g_strdup(just) : NULL;
                c->proposta_pendente = p;
                saida_linha(c, "[local] ferramenta sugerida: %s (parâmetros: %s) — use /plano para solicitar (executar) ou /simular",
                            p->ferramenta, p->parametros_json);
            }
        }
    }
}

static void tratar_msg_resposta(LiaChat *c, const LiaMsg *m)
{
    const char *texto = campo_str(m->dados, "resposta_texto");
    saida_linha(c, "[modelo, resumo] %s", texto ? texto : "");
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
    case LIA_MSG_AUDITORIA: saida_linha(c, "[auditoria registrada]"); break;
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
        if (!partes[1] || (strcmp(partes[1], "mock") && strcmp(partes[1], "ollama"))) {
            saida_linha(c, "[local] uso: /backend mock|ollama");
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
        if (c->interativo && !c->modo_json) {
            fputs("\033[H\033[2J", stdout);
            fflush(stdout);
            if (c->readline_ativo) rl_forced_update_display(); /* redesenha o prompt no topo da tela limpa */
        }
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

/* libedit (como o GNU Readline, cuja API ele imita) não tem um parâmetro
 * de dados do usuário no manipulador de linha completa — só esta
 * instância de LiaChat existe por processo, então um ponteiro global é
 * seguro e é exatamente o padrão recomendado pela própria lib para este
 * caso. */
static LiaChat *g_chat_atual;

static void ao_linha_completa(char *linha)
{
    LiaChat *c = g_chat_atual;
    if (!linha) {
        /* EOF (Ctrl-D): libedit já deixou o terminal em ordem. */
        finalizar(c);
        return;
    }
    if (*linha) add_history(linha); /* só em memória desta sessão — nunca gravado em disco */

    if (c->modo_json) processar_linha_json(c, linha);
    else processar_comando(c, linha);
    free(linha); /* libedit aloca com malloc, não g_malloc */
    flush_saida(c);
}

static gboolean ao_stdin_pronto(gint fd, GIOCondition condicao, gpointer dados)
{
    (void)condicao; (void)dados;
    /* rl_callback_read_char() só consome UM byte por chamada. Uma seta
     * chega como 3 bytes (ESC [ A) praticamente juntos no buffer do
     * kernel — se cada byte só for consumido numa volta FUTURA do laço
     * principal do GLib (em vez de todos agora, enquanto ainda há dados
     * disponíveis), o atraso real entre eles passa do timeout interno de
     * reconhecimento de sequência de escape do libedit, que então trata
     * cada byte como tecla solta em vez de "seta pra cima" — sintoma
     * visto na prática: em vez de recuperar o histórico, entrava em modo
     * de busca incremental (prompt "bck:"). Drena tudo que já chegou,
     * nesta mesma invocação, antes de voltar ao laço principal. */
    for (;;) {
        int disponivel = 0;
        if (ioctl(fd, FIONREAD, &disponivel) != 0 || disponivel <= 0)
            break;
        rl_callback_read_char();
    }
    return G_SOURCE_CONTINUE;
}

/* Modo não interativo (stdin redirecionado/pipe — scripts, testes de
 * integração): libedit continua "editando" e ecoando caractere a
 * caractere mesmo sem um terminal de verdade do outro lado, o que só
 * produz lixo visual numa saída que ninguém vai ler com os olhos. Sem
 * tty, não há edição nenhuma para ganhar — volta ao leitor de linha
 * simples de sempre, sem eco próprio, sem controle de cursor. */
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
        fprintf(stderr, "lia-chat: erro lendo o daemon: %s\n", erro->message);
        g_error_free(erro);
        if (!c->deve_sair) c->codigo_saida = 1;
        finalizar(c);
        return;
    }
    if (!linha) {
        fprintf(stderr, "lia-chat: daemon desconectou\n");
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
        (c->pergunta_em_andamento && !c->pergunta_abandonada_localmente)) {
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
        "uso: %s [--socket CAMINHO] [--backend mock|ollama] [--modelo NOME] [--json] [--help]\n"
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
    /* stdout é totalmente bufferizado por padrão quando não é um TTY
     * (redirecionado a arquivo/pipe) — sem isto, nada apareceria até o
     * processo terminar, mesmo em modo interativo real com stdout
     * redirecionado (ex.: dentro de outra ferramenta). Cada linha sai na
     * hora, sempre. */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

    LiaChat c = { 0 };
    c.backend_assumido = g_strdup("mock");
    c.arquivos = g_ptr_array_new_with_free_func((GDestroyNotify)arquivo_anexo_liberar);
    c.buffer_saida = g_string_new(NULL);
    c.codigo_saida = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            imprimir_uso(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            c.socket_caminho = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "mock") && strcmp(argv[i], "ollama")) {
                fprintf(stderr, "lia-chat: --backend precisa ser 'mock' ou 'ollama'\n");
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
        return 1;
    }
    /* o timeout do GSocketClient (acima) vale só para a tentativa de
     * conectar — GSocketClient propaga esse valor para o GSocket
     * resultante, e sem isto TODA leitura ociosa subsequente (esperar a
     * próxima mensagem do daemon, o normal entre uma pergunta e outra)
     * falharia com G_IO_ERROR_TIMED_OUT depois de 5s. Uma conversa não
     * tem prazo — zera para "sem timeout" assim que a conexão existe. */
    g_socket_set_timeout(g_socket_connection_get_socket(c.conexao), 0);

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
        flush_saida(&c); /* readline ainda não foi instalado aqui — sempre o caminho plain print */
    }

    if (c.interativo) {
        /* Edição de linha real (setas, histórico, Ctrl-A/E/K/W, copiar/
         * colar) via libedit em modo "callback": ela cuida do terminal
         * (raw mode, eco, redesenho) e só devolve o controle quando uma
         * linha completa chega ou dá EOF (Ctrl-D) — ao_linha_completa.
         * rl_catch_signals=0 porque o SIGINT já tem o dono certo
         * (g_unix_signal_add acima, instalado antes de qualquer saída);
         * sem isto o libedit instalaria o próprio handler de Ctrl-C e
         * brigaria com o nosso. */
        rl_catch_signals = 0;
        rl_catch_sigwinch = 1; /* redesenha certo quando o terminal é redimensionado */
        /* libedit, sem isto, usa modo vi por padrão (ESC entra em modo de
         * comando — "h"/"j"/"k"/"l" etc. deixam de inserir texto e viram
         * comandos de movimento, o que pareceria "quebrado" para qualquer
         * pessoa esperando um terminal normal). Modo emacs: digitar sempre
         * insere, setas movem o cursor/histórico, sem modos. */
        rl_variable_bind("editing-mode", "emacs");
        g_chat_atual = &c;
        rl_callback_handler_install(c.modo_json ? "" : "lia> ", ao_linha_completa);
        c.readline_ativo = TRUE;
        g_unix_fd_add(STDIN_FILENO, G_IO_IN, ao_stdin_pronto, &c);
    } else {
        /* stdin não é um terminal (pipe/arquivo/script, ou make
         * teste-chat): libedit só ecoaria lixo sem um tty de verdade do
         * outro lado — leitor de linha simples, sem edição nenhuma
         * (não há humano ali para precisar dela). */
        GInputStream *fluxo_stdin = g_unix_input_stream_new(STDIN_FILENO, FALSE);
        c.entrada_stdin = g_data_input_stream_new(fluxo_stdin);
        g_object_unref(fluxo_stdin);
        agendar_leitura_stdin_pipe(&c);
    }

    agendar_leitura_socket(&c);

    g_main_loop_run(c.loop);

    if (c.readline_ativo) rl_callback_handler_remove();
    if (c.entrada_stdin) g_object_unref(c.entrada_stdin);
    if (c.conexao) { g_io_stream_close(G_IO_STREAM(c.conexao), NULL, NULL); g_object_unref(c.conexao); }
    g_object_unref(c.entrada_socket);
    g_main_loop_unref(c.loop);
    lia_sessao_liberar(&c.sessao);
    g_ptr_array_free(c.arquivos, TRUE);
    g_string_free(c.buffer_saida, TRUE);
    if (c.proposta_pendente) proposta_pendente_liberar(c.proposta_pendente);
    if (c.plano_dados_pendente) json_object_unref(c.plano_dados_pendente);
    g_free(c.plano_id_pendente);
    g_free(c.ferramenta_plano_pendente);
    g_free(c.execucao_id_pendente);
    g_free(c.ultimo_id_enviado);
    g_free(c.socket_caminho);
    g_free(c.backend_assumido);
    g_free(c.modelo_assumido);

    return c.codigo_saida;
}
