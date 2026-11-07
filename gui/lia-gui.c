/* lia-gui — GUI mínima (GTK4), camada fina sobre docs/protocolo.md.
 *
 * Esta é a primeira etapa: SOMENTE LEITURA. A janela mostra o que um
 * daemon enviaria (quando existir um) e nunca envia 'solicitar'/'aprovar'
 * (gui/protocolo.c recusa isso localmente, mesmo que este arquivo tentasse
 * — ver lia_sessao_validar_envio). Não há daemon ainda (docs/protocolo.md,
 * "Status"), então o estado normal desta GUI, hoje, é desconectada — e ela
 * precisa mostrar isso com clareza, não falhar silenciosamente nem travar.
 *
 * Esta GUI NUNCA chama sudo/pkexec/system/exec*; ver teste em
 * gui/README-teste.md e a verificação automatizada de símbolos no binário
 * (make teste-gui / README.md).
 */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include "protocolo.h"

#define SOCKET_PROTOCOLO "/.lia/maquina/estado/sessao.sock"

typedef enum {
    ESTADO_DESCONECTADO,
    ESTADO_PLANEJANDO,
    ESTADO_AGUARDANDO_APROVACAO,
    ESTADO_EXECUTANDO,
    ESTADO_CONCLUIDO,
    ESTADO_ERRO,
} EstadoUi;

typedef struct {
    GtkWidget *janela;
    GtkWidget *rotulo_estado;
    GtkWidget *conversa;      /* GtkTextView, só leitura */
    GtkWidget *entrada;       /* GtkEntry, desabilitado nesta etapa */
    GtkWidget *lista_eventos; /* GtkListBox */
    GtkWidget *painel_permissoes; /* GtkLabel multi-linha */
    GtkWidget *botao_cancelar;
    LiaSessao sessao;
    GSocketConnection *conexao;     /* NULL quando desconectado */
    GDataInputStream *entrada_dados; /* mantém referência enquanto a leitura assíncrona estiver pendente */
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

/* Processa uma linha NDJSON recebida do daemon (ou, nos testes manuais,
 * injetada via stdin — ver README.md). Nunca decide nada: só valida,
 * atualiza a sessão e reflete na interface. */
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

    switch (m.tipo) {
    case LIA_MSG_PLANO:
        ui_marcar_estado(g, ESTADO_AGUARDANDO_APROVACAO, "aguardando aprovação");
        break;
    case LIA_MSG_PROGRESSO:
        ui_marcar_estado(g, ESTADO_EXECUTANDO, "executando");
        break;
    case LIA_MSG_RESULTADO:
        ui_marcar_estado(g, ESTADO_CONCLUIDO, "concluído");
        break;
    case LIA_MSG_ERRO:
        ui_marcar_estado(g, ESTADO_ERRO, "erro");
        break;
    default:
        break;
    }
    lia_msg_liberar(&m);
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
        ui_marcar_estado(g, ESTADO_DESCONECTADO, "desconectado (erro de leitura)");
        return;
    }
    if (!linha) {
        /* EOF: o daemon fechou a conexão do lado dele. */
        ui_append_conversa(g, "[local] daemon desconectou");
        lia_sessao_desconectar(&g->sessao);
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
    if (!g->conexao) {
        ui_append_conversa(g, "[local] cancelar recusado: sem conexão com o daemon");
        return;
    }
    /* Em uma próxima etapa: serializar {"tipo":"cancelar",...} e escrever
     * na conexão. Nesta PoC somente leitura, só registramos a intenção. */
    ui_append_conversa(g, "[local] pedido de cancelamento seria enviado ao daemon aqui");
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
        if (erro) g_error_free(erro);
        ui_marcar_estado(g, ESTADO_DESCONECTADO, "desconectado (sem servidor)");
        lia_sessao_desconectar(&g->sessao);
        return;
    }

    g->conexao = conexao;
    lia_sessao_conectar(&g->sessao);
    ui_marcar_estado(g, ESTADO_PLANEJANDO, "conectado");
    ui_append_conversa(g, "[local] conectado ao daemon");

    GInputStream *fluxo_entrada = g_io_stream_get_input_stream(G_IO_STREAM(conexao));
    g->entrada_dados = g_data_input_stream_new(fluxo_entrada);
    g_data_input_stream_read_line_async(g->entrada_dados, G_PRIORITY_DEFAULT, NULL, ao_ler_linha, g);
}

static void construir_janela(GtkApplication *app, gpointer dados)
{
    LiaGui *g = dados;
    lia_sessao_iniciar(&g->sessao);

    g->janela = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g->janela), "Lia (prova de conceito — somente leitura)");
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

    GtkWidget *linha_entrada = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g->entrada = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g->entrada),
        "Somente leitura nesta etapa: texto fica como rascunho local, não é enviado");
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
        "Modo somente leitura.\nNenhuma capacidade com efeito\né solicitada nesta etapa.\n"
        "Ver docs/manifesto.md.");
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
    return status;
}
