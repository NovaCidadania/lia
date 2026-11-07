/* lia-gui — GUI mínima (GTK4), camada fina sobre docs/protocolo.md.
 *
 * Pode enviar 'solicitar' só para as seis ferramentas de leitura do
 * executor real (docs/manifesto.md: tools.discover/list/probe,
 * sistema.diagnosticar/recursos/dispositivos) — todas risco=nenhum, sem
 * efeito observável fora do processo. NUNCA envia 'aprovar': nenhuma
 * dessas ferramentas chega a "aguardando aprovação", então a GUI não
 * precisa decidir isso (gui/protocolo.c recusa 'aprovar' localmente —
 * lia_sessao_validar_envio — mesmo que este arquivo tentasse). O daemon
 * continua sendo a única autoridade (ferramenta_desconhecida/
 * capacidade_negada/parametro_invalido são sempre decisão dele). O campo
 * de texto livre continua inerte (rascunho local, nunca enviado) — ele
 * não corresponde a nenhum pedido estruturado do manifesto.
 *
 * Sem daemon, a GUI mostra "desconectado" claramente e continua
 * funcional (nunca trava nem simula uma conexão).
 *
 * Esta GUI NUNCA chama sudo/pkexec/system/exec*; ver gui/teste_fumaca.sh
 * e a verificação automatizada de símbolos no binário (make gui-smoke).
 */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include "protocolo.h"

#define SOCKET_PROTOCOLO "/.lia/run/session.sock"

typedef enum {
    ESTADO_DESCONECTADO,
    ESTADO_PLANEJANDO,
    ESTADO_AGUARDANDO_APROVACAO,
    ESTADO_EXECUTANDO,
    ESTADO_CONCLUIDO,
    ESTADO_ERRO,
} EstadoUi;

/* As seis ferramentas de leitura que o executor real aceita nesta etapa
 * (sessiond/manifesto_real.c, docs/manifesto.md) — todas risco=nenhum,
 * então a GUI pode pedi-las diretamente (lia_sessao_validar_envio só
 * continua bloqueando 'aprovar', nunca necessário para estas). */
static const char *const FERRAMENTAS_REAIS[] = {
    "tools.discover", "tools.list", "tools.probe",
    "sistema.diagnosticar", "sistema.recursos", "sistema.dispositivos",
};
#define N_FERRAMENTAS_REAIS (sizeof(FERRAMENTAS_REAIS) / sizeof(FERRAMENTAS_REAIS[0]))

typedef struct {
    GtkWidget *janela;
    GtkWidget *rotulo_estado;
    GtkWidget *conversa;      /* GtkTextView, só leitura */
    GtkWidget *entrada;       /* GtkEntry, desabilitado nesta etapa (rascunho local) */
    GtkWidget *combo_ferramentas;
    GtkWidget *botao_solicitar;
    GtkWidget *lista_eventos; /* GtkListBox */
    GtkWidget *painel_permissoes; /* GtkLabel multi-linha: ferramenta/capacidade/risco/parâmetros do último plano */
    GtkWidget *botao_cancelar;
    LiaSessao sessao;
    GSocketConnection *conexao;     /* NULL quando desconectado */
    GOutputStream *saida;           /* válido junto com 'conexao' */
    GDataInputStream *entrada_dados; /* mantém referência enquanto a leitura assíncrona estiver pendente */
    char *execucao_id_atual;        /* da última 'plano'/'progresso' recebida, para o botão Cancelar */
} LiaGui;

static void ui_marcar_estado(LiaGui *g, EstadoUi e, const char *texto)
{
    const char *cor;
    switch (e) {
    case ESTADO_DESCONECTADO:          cor = "gray"; break;
    case ESTADO_PLANEJANDO:            cor = "blue"; break;
    case ESTADO_AGUARDANDO_APROVACAO:  cor = "orange"; break;
    case ESTADO_EXECUTANDO:            cor = "blue"; break;
    case ESTADO_CONCLUIDO:             cor = "green"; break;
    default:                           cor = "red"; break;
    }
    char marcado[256];
    g_snprintf(marcado, sizeof(marcado), "<span foreground=\"%s\"><b>%s</b></span>", cor, texto);
    gtk_label_set_markup(GTK_LABEL(g->rotulo_estado), marcado);
    gtk_widget_set_sensitive(g->botao_cancelar, e == ESTADO_EXECUTANDO || e == ESTADO_AGUARDANDO_APROVACAO);
}

static void ui_append_conversa(LiaGui *g, const char *linha)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g->conversa));
    GtkTextIter fim;
    gtk_text_buffer_get_end_iter(buf, &fim);
    gtk_text_buffer_insert(buf, &fim, linha, -1);
    gtk_text_buffer_insert(buf, &fim, "\n", -1);
}

static void ui_append_evento(LiaGui *g, const char *resumo)
{
    GtkWidget *linha = gtk_label_new(resumo);
    gtk_label_set_xalign(GTK_LABEL(linha), 0.0);
    gtk_list_box_append(GTK_LIST_BOX(g->lista_eventos), linha);
}

static const char *campo_str_ou(JsonObject *obj, const char *chave, const char *padrao)
{
    if (!obj || !json_object_has_member(obj, chave))
        return padrao;
    JsonNode *n = json_object_get_member(obj, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING)
        return padrao;
    return json_node_get_string(n);
}

/* Mostra ferramenta escolhida, capacidade exigida, risco e parâmetros
 * resumidos de um 'plano' recebido — nunca decide nada a partir disso,
 * só exibe o que o daemon já decidiu (docs/protocolo.md §3). */
static void ui_mostrar_plano(LiaGui *g, JsonObject *dados)
{
    if (!dados)
        return;
    GString *texto = g_string_new(NULL);
    g_string_append_printf(texto, "Ferramenta:  %s\n", campo_str_ou(dados, "ferramenta", "?"));
    g_string_append_printf(texto, "Risco:       %s\n", campo_str_ou(dados, "risco", "?"));
    g_string_append_printf(texto, "Reversível:  %s\n", campo_str_ou(dados, "reversivel", "?"));

    if (json_object_has_member(dados, "capacidades")) {
        JsonNode *n = json_object_get_member(dados, "capacidades");
        g_string_append(texto, "Capacidades: ");
        if (JSON_NODE_TYPE(n) == JSON_NODE_ARRAY) {
            JsonArray *arr = json_node_get_array(n);
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                if (i) g_string_append(texto, ", ");
                g_string_append(texto, json_array_get_string_element(arr, i));
            }
        }
        g_string_append_c(texto, '\n');
    }
    if (json_object_has_member(dados, "parametros_normalizados")) {
        JsonNode *n = json_object_get_member(dados, "parametros_normalizados");
        JsonGenerator *gen = json_generator_new();
        json_generator_set_root(gen, n);
        char *txt = json_generator_to_data(gen, NULL);
        g_string_append_printf(texto, "Parâmetros:  %s\n", txt);
        g_free(txt);
        g_object_unref(gen);
    }
    g_string_append_printf(texto, "requer_aprovacao: %s",
        json_object_has_member(dados, "requer_aprovacao") &&
        json_object_get_boolean_member(dados, "requer_aprovacao") ? "true" : "false");

    gtk_label_set_text(GTK_LABEL(g->painel_permissoes), texto->str);
    g_string_free(texto, TRUE);
}

/* Processa uma linha NDJSON recebida do daemon. Nunca decide nada: só
 * valida, atualiza a sessão e reflete na interface. */
static void processar_linha_recebida(LiaGui *g, const char *linha)
{
    LiaMsg m;
    LiaMsgErro erro_msg = lia_msg_parsear(linha, &m);
    if (erro_msg != LIA_MSG_OK) {
        char texto[512];
        g_snprintf(texto, sizeof(texto), "[protocolo] linha rejeitada: %s", lia_msg_erro_texto(erro_msg));
        ui_append_evento(g, texto);
        ui_marcar_estado(g, ESTADO_ERRO, "erro de protocolo");
        return;
    }

    LiaSessaoErro erro_sessao = lia_sessao_receber(&g->sessao, &m);
    char resumo[512];
    g_snprintf(resumo, sizeof(resumo), "%s id=%s", lia_msg_tipo_para_string(m.tipo), m.id);
    ui_append_evento(g, resumo);

    if (erro_sessao != LIA_SESSAO_OK) {
        char texto[256];
        g_snprintf(texto, sizeof(texto), "[sessão] mensagem fora de ordem/inválida (código %d) — ignorada", erro_sessao);
        ui_append_conversa(g, texto);
        ui_marcar_estado(g, ESTADO_ERRO, "mensagem fora de ordem");
        lia_msg_liberar(&m);
        return;
    }

    if (m.dados && json_object_has_member(m.dados, "execucao_id")) {
        g_free(g->execucao_id_atual);
        g->execucao_id_atual = g_strdup(campo_str_ou(m.dados, "execucao_id", NULL));
    }

    switch (m.tipo) {
    case LIA_MSG_PLANO:
        ui_marcar_estado(g, ESTADO_AGUARDANDO_APROVACAO, "plano recebido");
        ui_mostrar_plano(g, m.dados);
        ui_append_conversa(g, "[plano] ver painel de permissões à direita");
        break;
    case LIA_MSG_PROGRESSO: {
        ui_marcar_estado(g, ESTADO_EXECUTANDO, "executando");
        char texto[300];
        g_snprintf(texto, sizeof(texto), "[progresso] %s (%.0f%%)",
                   campo_str_ou(m.dados, "mensagem", ""),
                   json_object_has_member(m.dados, "fracao")
                       ? json_object_get_double_member(m.dados, "fracao") * 100.0 : 0.0);
        ui_append_conversa(g, texto);
        break;
    }
    case LIA_MSG_SAIDA: {
        char texto[600];
        g_snprintf(texto, sizeof(texto), "[saída] %s", campo_str_ou(m.dados, "trecho", ""));
        ui_append_conversa(g, texto);
        break;
    }
    case LIA_MSG_RESULTADO: {
        const char *estado = campo_str_ou(m.dados, "estado", "?");
        char texto[300];
        g_snprintf(texto, sizeof(texto), "[resultado] estado=%s codigo_saida=%" G_GINT64_FORMAT,
                   estado,
                   json_object_has_member(m.dados, "codigo_saida")
                       ? json_object_get_int_member(m.dados, "codigo_saida") : 0);
        ui_append_conversa(g, texto);
        ui_marcar_estado(g, ESTADO_CONCLUIDO, strcmp(estado, "sucesso") == 0 ? "concluído" : estado);
        g_free(g->execucao_id_atual);
        g->execucao_id_atual = NULL;
        break;
    }
    case LIA_MSG_ERRO: {
        char texto[400];
        g_snprintf(texto, sizeof(texto), "[erro] %s: %s",
                   campo_str_ou(m.dados, "codigo", "?"), campo_str_ou(m.dados, "mensagem", ""));
        ui_append_conversa(g, texto);
        ui_marcar_estado(g, ESTADO_ERRO, "erro");
        break;
    }
    default:
        break;
    }
    lia_msg_liberar(&m);
}

/* Monta e escreve uma mensagem no socket, depois de validar localmente
 * (defesa em profundidade — o daemon continua sendo a autoridade real). */
static gboolean enviar_mensagem(LiaGui *g, LiaMsgTipo tipo, JsonBuilder *dados)
{
    LiaEnvioErro erro = lia_sessao_validar_envio(&g->sessao, tipo, NULL, 0);
    if (erro != LIA_ENVIO_OK) {
        char texto[200];
        g_snprintf(texto, sizeof(texto), "[local] envio recusado localmente (código %d)", erro);
        ui_append_conversa(g, texto);
        return FALSE;
    }
    if (!g->conexao) {
        ui_append_conversa(g, "[local] envio recusado: sem conexão com o daemon");
        return FALSE;
    }
    char *id = lia_novo_id("gui");
    char *linha = lia_msg_montar(tipo, id, NULL, dados);
    g_free(id);
    GError *erro_io = NULL;
    gboolean ok = g_output_stream_write_all(g->saida, linha, strlen(linha), NULL, NULL, &erro_io);
    if (!ok) {
        char texto[300];
        g_snprintf(texto, sizeof(texto), "[local] falha ao escrever no socket: %s",
                   erro_io ? erro_io->message : "?");
        ui_append_conversa(g, texto);
        if (erro_io) g_error_free(erro_io);
    }
    g_free(linha);
    return ok;
}

static void ao_ler_linha(GObject *origem, GAsyncResult *res, gpointer dados)
{
    LiaGui *g = dados;
    GError *erro = NULL;
    gsize tamanho = 0;
    gchar *linha = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(origem), res, &tamanho, &erro);

    if (erro) {
        ui_append_conversa(g, "[local] erro de leitura do daemon — tratando como desconexão");
        g_error_free(erro);
        lia_sessao_desconectar(&g->sessao);
        g->conexao = NULL;
        g->saida = NULL;
        ui_marcar_estado(g, ESTADO_DESCONECTADO, "desconectado (erro de leitura)");
        return;
    }
    if (!linha) {
        /* EOF: o daemon fechou a conexão do lado dele. */
        ui_append_conversa(g, "[local] daemon desconectou");
        lia_sessao_desconectar(&g->sessao);
        g->conexao = NULL;
        g->saida = NULL;
        ui_marcar_estado(g, ESTADO_DESCONECTADO, "desconectado (daemon encerrou)");
        return;
    }

    processar_linha_recebida(g, linha);
    g_free(linha);
    g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(origem), G_PRIORITY_DEFAULT, NULL, ao_ler_linha, g);
}

static void ao_clicar_cancelar(GtkButton *btn, gpointer dados)
{
    (void)btn;
    LiaGui *g = dados;
    LiaEnvioErro erro = lia_sessao_validar_envio(&g->sessao, LIA_MSG_CANCELAR, NULL, 0);
    if (erro != LIA_ENVIO_OK) {
        ui_append_conversa(g, "[local] cancelar recusado: nada em andamento para cancelar");
        return;
    }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "execucao_id");
    json_builder_add_string_value(b, g->execucao_id_atual ? g->execucao_id_atual : "");
    json_builder_end_object(b);
    if (enviar_mensagem(g, LIA_MSG_CANCELAR, b))
        ui_append_conversa(g, "[local] pedido de cancelamento enviado");
    g_object_unref(b);
}

static void ao_clicar_solicitar(GtkButton *btn, gpointer dados)
{
    (void)btn;
    LiaGui *g = dados;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(g->combo_ferramentas));
    if (idx == GTK_INVALID_LIST_POSITION || idx >= N_FERRAMENTAS_REAIS) {
        ui_append_conversa(g, "[local] escolha uma ferramenta antes de solicitar");
        return;
    }
    const char *ferramenta = FERRAMENTAS_REAIS[idx];
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "ferramenta");
    json_builder_add_string_value(b, ferramenta);
    json_builder_set_member_name(b, "parametros");
    json_builder_begin_object(b);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "modo");
    json_builder_add_string_value(b, "executar"); /* as seis ferramentas de leitura leem de verdade, nunca simulam */
    json_builder_end_object(b);
    if (enviar_mensagem(g, LIA_MSG_SOLICITAR, b)) {
        char texto[200];
        g_snprintf(texto, sizeof(texto), "[local] solicitar '%s' enviado", ferramenta);
        ui_append_conversa(g, texto);
        ui_marcar_estado(g, ESTADO_PLANEJANDO, "aguardando plano");
    }
    g_object_unref(b);
}

static void ao_ativar_entrada(GtkEntry *entrada, gpointer dados)
{
    LiaGui *g = dados;
    const char *texto = gtk_editable_get_text(GTK_EDITABLE(entrada));
    if (!texto || !*texto)
        return;
    /* Somente leitura nesta etapa: nunca constrói nem envia 'solicitar'.
     * O texto só é ecoado localmente como rascunho — prova de que o campo
     * funciona, sem nenhum efeito no sistema. */
    char eco[600];
    g_snprintf(eco, sizeof(eco), "[rascunho, não enviado — modo somente leitura] %s", texto);
    ui_append_conversa(g, eco);
    gtk_editable_set_text(GTK_EDITABLE(entrada), "");
}

/* Tenta conectar ao socket do protocolo (docs/protocolo.md §1). Se não
 * houver daemon — o caso normal hoje —, fica em modo sem servidor: a GUI
 * continua funcional, só não recebe eventos. */
static void tentar_conectar(LiaGui *g)
{
    const char *home = g_get_home_dir();
    char caminho[1024];
    g_snprintf(caminho, sizeof(caminho), "%s%s", home, SOCKET_PROTOCOLO);

    GSocketAddress *endereco = g_unix_socket_address_new(caminho);
    GSocketClient *cliente = g_socket_client_new();
    GError *erro = NULL;
    GSocketConnection *conexao = g_socket_client_connect(cliente, G_SOCKET_CONNECTABLE(endereco), NULL, &erro);
    g_object_unref(endereco);
    g_object_unref(cliente);

    if (!conexao) {
        char texto[600];
        g_snprintf(texto, sizeof(texto), "[local] sem daemon em %s (%s) — modo sem servidor",
                   caminho, erro ? erro->message : "?");
        ui_append_conversa(g, texto);
        g_print("lia-gui: %s\n", texto);
        if (erro) g_error_free(erro);
        ui_marcar_estado(g, ESTADO_DESCONECTADO, "desconectado (sem servidor)");
        lia_sessao_desconectar(&g->sessao);
        return;
    }

    g->conexao = conexao;
    g->saida = g_io_stream_get_output_stream(G_IO_STREAM(conexao));
    lia_sessao_conectar(&g->sessao);
    ui_marcar_estado(g, ESTADO_PLANEJANDO, "conectado");
    ui_append_conversa(g, "[local] conectado ao daemon");
    g_print("lia-gui: conectado ao daemon em %s\n", caminho);

    GInputStream *fluxo_entrada = g_io_stream_get_input_stream(G_IO_STREAM(conexao));
    g->entrada_dados = g_data_input_stream_new(fluxo_entrada);
    g_data_input_stream_read_line_async(g->entrada_dados, G_PRIORITY_DEFAULT, NULL, ao_ler_linha, g);
}

static void construir_janela(GtkApplication *app, gpointer dados)
{
    LiaGui *g = dados;
    lia_sessao_iniciar(&g->sessao);

    g->janela = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g->janela), "Lia (prova de conceito — ferramentas de leitura)");
    gtk_window_set_default_size(GTK_WINDOW(g->janela), 760, 520);

    GtkWidget *raiz = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(raiz, 8);
    gtk_widget_set_margin_end(raiz, 8);
    gtk_widget_set_margin_top(raiz, 8);
    gtk_widget_set_margin_bottom(raiz, 8);
    gtk_window_set_child(GTK_WINDOW(g->janela), raiz);

    /* barra de estado */
    GtkWidget *barra_estado = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *rotulo_fixo = gtk_label_new("Estado:");
    g->rotulo_estado = gtk_label_new(NULL);
    gtk_box_append(GTK_BOX(barra_estado), rotulo_fixo);
    gtk_box_append(GTK_BOX(barra_estado), g->rotulo_estado);
    gtk_box_append(GTK_BOX(raiz), barra_estado);

    /* corpo: conversa+entrada à esquerda, eventos+permissões à direita */
    GtkWidget *corpo = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(corpo, TRUE);
    gtk_box_append(GTK_BOX(raiz), corpo);

    GtkWidget *coluna_esquerda = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *rolagem_conversa = gtk_scrolled_window_new();
    g->conversa = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g->conversa), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(g->conversa), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g->conversa), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rolagem_conversa), g->conversa);
    gtk_widget_set_vexpand(rolagem_conversa, TRUE);
    gtk_box_append(GTK_BOX(coluna_esquerda), rolagem_conversa);

    /* seletor das seis ferramentas de leitura reais (docs/manifesto.md) +
     * Solicitar — a única ação com efeito real que a GUI pode disparar
     * nesta etapa, porque todas têm risco=nenhum (ver protocolo.c). */
    GtkWidget *linha_ferramentas = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkStringList *modelo_ferramentas = gtk_string_list_new(NULL);
    for (size_t i = 0; i < N_FERRAMENTAS_REAIS; i++)
        gtk_string_list_append(modelo_ferramentas, FERRAMENTAS_REAIS[i]);
    g->combo_ferramentas = gtk_drop_down_new(G_LIST_MODEL(modelo_ferramentas), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g->combo_ferramentas), 0);
    gtk_widget_set_hexpand(g->combo_ferramentas, TRUE);
    g->botao_solicitar = gtk_button_new_with_label("Solicitar");
    g_signal_connect(g->botao_solicitar, "clicked", G_CALLBACK(ao_clicar_solicitar), g);
    gtk_box_append(GTK_BOX(linha_ferramentas), g->combo_ferramentas);
    gtk_box_append(GTK_BOX(linha_ferramentas), g->botao_solicitar);
    gtk_box_append(GTK_BOX(coluna_esquerda), linha_ferramentas);

    GtkWidget *linha_entrada = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g->entrada = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g->entrada),
        "Rascunho local, não enviado — use o seletor de ferramentas acima para pedidos reais");
    gtk_widget_set_hexpand(g->entrada, TRUE);
    g_signal_connect(g->entrada, "activate", G_CALLBACK(ao_ativar_entrada), g);
    g->botao_cancelar = gtk_button_new_with_label("Cancelar");
    gtk_widget_set_sensitive(g->botao_cancelar, FALSE);
    g_signal_connect(g->botao_cancelar, "clicked", G_CALLBACK(ao_clicar_cancelar), g);
    gtk_box_append(GTK_BOX(linha_entrada), g->entrada);
    gtk_box_append(GTK_BOX(linha_entrada), g->botao_cancelar);
    gtk_box_append(GTK_BOX(coluna_esquerda), linha_entrada);

    gtk_paned_set_start_child(GTK_PANED(corpo), coluna_esquerda);
    gtk_paned_set_resize_start_child(GTK_PANED(corpo), TRUE);

    GtkWidget *coluna_direita = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(coluna_direita, 260, -1);

    GtkWidget *rotulo_eventos = gtk_label_new("Eventos");
    gtk_label_set_xalign(GTK_LABEL(rotulo_eventos), 0.0);
    gtk_box_append(GTK_BOX(coluna_direita), rotulo_eventos);
    GtkWidget *rolagem_eventos = gtk_scrolled_window_new();
    g->lista_eventos = gtk_list_box_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rolagem_eventos), g->lista_eventos);
    gtk_widget_set_vexpand(rolagem_eventos, TRUE);
    gtk_box_append(GTK_BOX(coluna_direita), rolagem_eventos);

    GtkWidget *rotulo_permissoes = gtk_label_new("Permissões / capacidades");
    gtk_label_set_xalign(GTK_LABEL(rotulo_permissoes), 0.0);
    gtk_box_append(GTK_BOX(coluna_direita), rotulo_permissoes);
    g->painel_permissoes = gtk_label_new(
        "Nenhum plano ainda.\nSelecione uma ferramenta e clique\nSolicitar — todas de leitura,\n"
        "risco=nenhum (docs/manifesto.md).");
    gtk_label_set_xalign(GTK_LABEL(g->painel_permissoes), 0.0);
    gtk_label_set_wrap(GTK_LABEL(g->painel_permissoes), TRUE);
    gtk_box_append(GTK_BOX(coluna_direita), g->painel_permissoes);

    gtk_paned_set_end_child(GTK_PANED(corpo), coluna_direita);

    gtk_window_present(GTK_WINDOW(g->janela));
    ui_marcar_estado(g, ESTADO_DESCONECTADO, "iniciando");
    tentar_conectar(g);
}

int main(int argc, char **argv)
{
    LiaGui g = { 0 };
    GtkApplication *app = gtk_application_new("org.novacidadania.lia.gui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(construir_janela), &g);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    lia_sessao_liberar(&g.sessao);
    g_free(g.execucao_id_atual);
    return status;
}
