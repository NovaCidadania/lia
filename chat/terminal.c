/* lia-chat — implementação de chat/terminal.h. Ver o cabeçalho. */
#define _DEFAULT_SOURCE
#include "terminal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define TERM_HISTORICO_MAX 200
#define TERM_LINHA_MAX     (8 * 1024) /* igual a LIA_PROMPT_LIMITE_PEDIDO_BYTES: nada maior seria enviado mesmo */
#define TERM_SEPARADOR_COLS 30
#define TERM_SPINNER_MS    120

typedef enum { ESC_NADA = 0, ESC_ESC, ESC_CSI, ESC_SS3 } EscEstado;

struct Term {
    int fd_in, fd_out;
    struct termios termios_original;
    gboolean termios_salvo;
    gboolean utf8, cor;
    TermLinhaCb cb;
    gpointer dados;

    char *prompt;
    GString *linha;   /* texto em edição (UTF-8) */
    gsize cursor;     /* deslocamento em bytes dentro de 'linha' */
    gsize desloc;     /* primeiro byte visível quando a linha não cabe na largura */

    GPtrArray *historico;
    int hist_pos;     /* -1 = editando a linha nova; senão índice em 'historico' */
    char *rascunho;   /* linha nova guardada enquanto se navega no histórico */

    /* parser de entrada */
    EscEstado esc;
    GString *csi;
    gboolean colando;
    char utf8_pend[8];
    int utf8_n, utf8_falta;

    /* região viva */
    gboolean desenhado;
    int cols;
    int larg_status, larg_sep, larg_entrada; /* larguras (colunas) do que foi desenhado por último */
    int col_cursor;

    char *painel[TERM_PAINEL_MAX];  /* linhas do painel da tarefa (acima do status) */
    int n_painel;
    int larg_painel[TERM_PAINEL_MAX]; /* larguras do que foi desenhado por último */
    int n_painel_desenhado;

    char *status;
    gboolean ativo;
    gint64 ativo_desde_us;
    guint timer_spinner;
    guint quadro;
};

/* ------------------------------------------------------------ saída --- */

static void escrever(Term *t, const char *dados, gsize n)
{
    while (n > 0) {
        ssize_t r = write(t->fd_out, dados, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { g_usleep(1000); continue; }
            return; /* terminal sumiu: nada mais a fazer aqui */
        }
        dados += r;
        n -= (gsize)r;
    }
}

static int largura_terminal(Term *t)
{
    struct winsize ws;
    if (ioctl(t->fd_out, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20)
        return ws.ws_col;
    return 80;
}

static int largura_char(gunichar ch)
{
    if (ch == 0 || g_unichar_iszerowidth(ch) || g_unichar_ismark(ch)) return 0;
    if (ch < 0x20 || ch == 0x7f) return 0;
    return g_unichar_iswide(ch) ? 2 : 1;
}

/* largura visível de um texto que pode conter sequências SGR (ESC [ ... m) */
static int largura_texto(const char *s)
{
    int w = 0;
    for (const char *p = s; *p;) {
        if (*p == 0x1b && p[1] == '[') {
            p += 2;
            while (*p && !(*p >= 0x40 && *p <= 0x7e)) p++;
            if (*p) p++;
            continue;
        }
        w += largura_char(g_utf8_get_char(p));
        p = g_utf8_next_char(p);
    }
    return w;
}

/* copia 's' para 'out' cortando em 'max' colunas (preserva sequências SGR) */
static int anexar_cortado(GString *out, const char *s, int max)
{
    int w = 0;
    for (const char *p = s; *p;) {
        if (*p == 0x1b && p[1] == '[') {
            const char *q = p + 2;
            while (*q && !(*q >= 0x40 && *q <= 0x7e)) q++;
            if (*q) q++;
            g_string_append_len(out, p, q - p);
            p = q;
            continue;
        }
        gunichar ch = g_utf8_get_char(p);
        int lw = largura_char(ch);
        if (w + lw > max) break;
        const char *prox = g_utf8_next_char(p);
        g_string_append_len(out, p, prox - p);
        w += lw;
        p = prox;
    }
    return w;
}

/* --------------------------------------------------------- região viva --- */

static const char *const QUADROS_UTF8[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
static const char *const QUADROS_ASCII[] = { "|", "/", "-", "\\" };

static void compor_status(Term *t, GString *out)
{
    GString *bruto = g_string_new(NULL);
    if (t->ativo) {
        const char *quadro = t->utf8 ? QUADROS_UTF8[t->quadro % G_N_ELEMENTS(QUADROS_UTF8)]
                                     : QUADROS_ASCII[t->quadro % G_N_ELEMENTS(QUADROS_ASCII)];
        gint64 s = (g_get_monotonic_time() - t->ativo_desde_us) / G_USEC_PER_SEC;
        if (t->cor) g_string_append(bruto, "\033[36m");
        g_string_append_printf(bruto, "%s %s", quadro, t->status ? t->status : "");
        if (t->cor) g_string_append(bruto, "\033[0;2m");
        g_string_append_printf(bruto, " %s %" G_GINT64_FORMAT "s %s Ctrl-C cancela", t->utf8 ? "·" : "-", s, t->utf8 ? "·" : "-");
        if (t->cor) g_string_append(bruto, "\033[0m");
    } else {
        if (t->cor) g_string_append(bruto, "\033[2m");
        g_string_append_printf(bruto, "%s %s", t->utf8 ? "●" : "*", t->status ? t->status : "");
        if (t->cor) g_string_append(bruto, "\033[0m");
    }
    t->larg_status = anexar_cortado(out, bruto->str, t->cols - 1);
    if (t->cor) g_string_append(out, "\033[0m");
    g_string_free(bruto, TRUE);
}

/* Parte visível da linha em edição: quando não cabe, mostra uma janela em
 * volta do cursor (a linha de entrada ocupa sempre UMA linha do terminal). */
static void compor_entrada(Term *t, GString *out)
{
    int larg_prompt = largura_texto(t->prompt);
    int disponivel = t->cols - 1 - larg_prompt;
    if (disponivel < 8) disponivel = 8;

    if (t->desloc > t->cursor) t->desloc = t->cursor;
    for (;;) { /* avança a janela até o cursor caber */
        int w = 0;
        for (const char *p = t->linha->str + t->desloc; p < t->linha->str + t->cursor; p = g_utf8_next_char(p))
            w += largura_char(g_utf8_get_char(p));
        if (w <= disponivel - 2 || t->desloc >= t->cursor) break;
        t->desloc = (gsize)(g_utf8_next_char(t->linha->str + t->desloc) - t->linha->str);
    }
    if (largura_texto(t->linha->str) <= disponivel - 2) t->desloc = 0;

    if (t->cor) g_string_append(out, "\033[1m");
    g_string_append(out, t->prompt);
    if (t->cor) g_string_append(out, "\033[0m");
    int w = 0, col_cursor = larg_prompt;
    if (t->desloc > 0) {
        g_string_append(out, t->utf8 ? "…" : "<");
        w = 1;
    }
    const char *p = t->linha->str + t->desloc;
    gboolean cortou = FALSE;
    while (*p) {
        if (p == t->linha->str + t->cursor) col_cursor = larg_prompt + w;
        gunichar ch = g_utf8_get_char(p);
        int lw = largura_char(ch);
        if (w + lw > disponivel - 1) { cortou = TRUE; break; }
        const char *prox = g_utf8_next_char(p);
        if (ch < 0x20 || ch == 0x7f) g_string_append_c(out, '?');
        else g_string_append_len(out, p, prox - p);
        w += lw ? lw : (ch < 0x20 || ch == 0x7f ? 1 : 0);
        p = prox;
    }
    if (p == t->linha->str + t->cursor) col_cursor = larg_prompt + w;
    if (cortou) { g_string_append(out, t->utf8 ? "…" : ">"); w++; }
    t->larg_entrada = larg_prompt + w;
    t->col_cursor = col_cursor;
}

/* acrescenta em 'out' as sequências que apagam a região viva e deixam o
 * cursor no começo da primeira linha dela */
static void compor_apagar(Term *t, GString *out)
{
    if (!t->desenhado) return;
    /* linhas físicas acima do cursor: normalmente 2 (status + separador).
     * Se o terminal encolheu desde o último desenho e redistribuiu o texto
     * (reflow), cada linha lógica pode ocupar mais de uma física. */
    int cols = t->cols > 0 ? t->cols : 80;
    int acima = (t->larg_status > 0 ? (t->larg_status - 1) / cols : 0) + 1 +
                (t->larg_sep > 0 ? (t->larg_sep - 1) / cols : 0) + 1 +
                (t->col_cursor > 0 ? t->col_cursor / cols : 0);
    for (int i = 0; i < t->n_painel_desenhado; i++)
        acima += (t->larg_painel[i] > 0 ? (t->larg_painel[i] - 1) / cols : 0) + 1;
    g_string_append_printf(out, "\r\033[%dA\033[J", acima);
    t->desenhado = FALSE;
}

static void compor_desenhar(Term *t, GString *out)
{
    for (int i = 0; i < t->n_painel; i++) {
        t->larg_painel[i] = anexar_cortado(out, t->painel[i], t->cols - 1);
        if (t->cor) g_string_append(out, "\033[0m");
        g_string_append(out, "\033[K\r\n");
    }
    t->n_painel_desenhado = t->n_painel;
    compor_status(t, out);
    g_string_append(out, "\033[K\r\n");
    int n = MIN(TERM_SEPARADOR_COLS, t->cols - 1);
    if (t->cor) g_string_append(out, "\033[2m");
    for (int i = 0; i < n; i++) g_string_append(out, t->utf8 ? "─" : "-");
    if (t->cor) g_string_append(out, "\033[0m");
    t->larg_sep = n;
    g_string_append(out, "\033[K\r\n");
    compor_entrada(t, out);
    g_string_append(out, "\033[K\r");
    if (t->col_cursor > 0) g_string_append_printf(out, "\033[%dC", t->col_cursor);
    t->desenhado = TRUE;
}

static void redesenhar(Term *t)
{
    GString *out = g_string_new(NULL);
    compor_apagar(t, out);
    compor_desenhar(t, out);
    escrever(t, out->str, out->len);
    g_string_free(out, TRUE);
}

/* só a linha de status (spinner/relógio): não mexe no resto, não move o
 * cursor de onde a pessoa está digitando */
static void redesenhar_status(Term *t)
{
    if (!t->desenhado) return;
    GString *out = g_string_new("\0337"); /* salva cursor */
    int acima = 2 + t->col_cursor / (t->cols > 0 ? t->cols : 80);
    g_string_append_printf(out, "\r\033[%dA", acima);
    compor_status(t, out);
    g_string_append(out, "\033[K\0338"); /* restaura cursor */
    escrever(t, out->str, out->len);
    g_string_free(out, TRUE);
}

static gboolean ao_tick_spinner(gpointer dados)
{
    Term *t = dados;
    t->quadro++;
    redesenhar_status(t);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------- API --- */

Term *term_novo(int fd_entrada, int fd_saida, const char *prompt, TermLinhaCb cb, gpointer dados)
{
    if (!isatty(fd_entrada))
        return NULL;
    Term *t = g_new0(Term, 1);
    t->fd_in = fd_entrada;
    t->fd_out = fd_saida;
    t->cb = cb;
    t->dados = dados;
    t->prompt = g_strdup(prompt ? prompt : "> ");
    t->linha = g_string_new(NULL);
    t->csi = g_string_new(NULL);
    t->historico = g_ptr_array_new_with_free_func(g_free);
    t->hist_pos = -1;
    t->utf8 = g_get_charset(NULL);
    const char *term = g_getenv("TERM");
    t->cor = isatty(fd_saida) && term && *term && strcmp(term, "dumb") != 0 && !g_getenv("NO_COLOR");
    t->cols = largura_terminal(t);
    t->status = g_strdup("pronto");

    if (tcgetattr(fd_entrada, &t->termios_original) == 0) {
        t->termios_salvo = TRUE;
        struct termios cru = t->termios_original;
        cru.c_lflag &= ~(tcflag_t)(ICANON | ECHO | IEXTEN); /* ISIG fica: Ctrl-C -> SIGINT */
        cru.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR);
        cru.c_cc[VMIN] = 1;
        cru.c_cc[VTIME] = 0;
        tcsetattr(fd_entrada, TCSADRAIN, &cru);
    }
    escrever(t, "\033[?2004h", 8); /* bracketed paste */
    redesenhar(t);
    return t;
}

void term_liberar(Term *t)
{
    if (!t) return;
    if (t->timer_spinner) g_source_remove(t->timer_spinner);
    GString *out = g_string_new(NULL);
    compor_apagar(t, out);
    g_string_append(out, "\033[?2004l");
    escrever(t, out->str, out->len);
    g_string_free(out, TRUE);
    if (t->termios_salvo)
        tcsetattr(t->fd_in, TCSADRAIN, &t->termios_original);
    for (int i = 0; i < t->n_painel; i++) g_free(t->painel[i]);
    g_free(t->prompt);
    g_free(t->status);
    g_free(t->rascunho);
    g_string_free(t->linha, TRUE);
    g_string_free(t->csi, TRUE);
    g_ptr_array_free(t->historico, TRUE);
    g_free(t);
}

gboolean term_tem_cor(const Term *t) { return t && t->cor; }

void term_imprimir(Term *t, const char *texto)
{
    GString *out = g_string_new(NULL);
    compor_apagar(t, out);
    /* sem "apagar até o fim da linha" depois de cada linha de texto: numa
     * linha que ocupa exatamente a largura do terminal, o cursor fica
     * "pendurado" na última coluna e esse comando apagaria o último
     * caractere. Não é preciso: compor_apagar já limpou tudo daqui para
     * baixo. */
    for (const char *p = texto ? texto : ""; *p; p++) {
        if (*p == '\n') g_string_append(out, "\r\n");
        else if (*p != '\r') g_string_append_c(out, *p);
    }
    gsize n = texto ? strlen(texto) : 0;
    if (n == 0 || texto[n - 1] != '\n')
        g_string_append(out, "\r\n");
    if (t->cor) g_string_append(out, "\033[0m");
    compor_desenhar(t, out);
    escrever(t, out->str, out->len); /* uma escrita só: nunca um quadro pela metade na tela */
    g_string_free(out, TRUE);
}

void term_definir_status(Term *t, const char *texto, gboolean ativo, gboolean continuar_relogio)
{
    g_free(t->status);
    t->status = g_strdup(texto ? texto : "");
    if (ativo && !(t->ativo && continuar_relogio))
        t->ativo_desde_us = g_get_monotonic_time();
    t->ativo = ativo;
    if (ativo && !t->timer_spinner)
        t->timer_spinner = g_timeout_add(TERM_SPINNER_MS, ao_tick_spinner, t);
    if (!ativo && t->timer_spinner) {
        g_source_remove(t->timer_spinner);
        t->timer_spinner = 0;
    }
    redesenhar_status(t);
}

void term_definir_painel(Term *t, const char *texto)
{
    for (int i = 0; i < t->n_painel; i++) g_clear_pointer(&t->painel[i], g_free);
    t->n_painel = 0;
    if (texto && *texto) {
        char **linhas = g_strsplit(texto, "\n", -1);
        for (int i = 0; linhas[i] && t->n_painel < TERM_PAINEL_MAX; i++)
            if (*linhas[i]) t->painel[t->n_painel++] = g_strdup(linhas[i]);
        g_strfreev(linhas);
    }
    redesenhar(t);
}

void term_definir_prompt(Term *t, const char *prompt)
{
    if (g_strcmp0(t->prompt, prompt) == 0) return;
    g_free(t->prompt);
    t->prompt = g_strdup(prompt ? prompt : "> ");
    redesenhar(t);
}

void term_redimensionado(Term *t)
{
    /* compor_apagar usa a NOVA largura para estimar quantas linhas físicas
     * o desenho antigo ocupa agora (ver comentário lá). */
    t->cols = largura_terminal(t);
    redesenhar(t);
}

void term_limpar_tela(Term *t)
{
    t->desenhado = FALSE;
    escrever(t, "\033[H\033[2J", 7);
    redesenhar(t);
}

/* ----------------------------------------------------------- edição --- */

static void inserir(Term *t, const char *s, gsize n)
{
    if (t->linha->len + n > TERM_LINHA_MAX) return; /* limite duro: nunca cresce sem fim (colagem enorme) */
    g_string_insert_len(t->linha, (gssize)t->cursor, s, (gssize)n);
    t->cursor += n;
}

static void mover_esquerda(Term *t)
{
    if (t->cursor == 0) return;
    t->cursor = (gsize)(g_utf8_prev_char(t->linha->str + t->cursor) - t->linha->str);
}

static void mover_direita(Term *t)
{
    if (t->cursor >= t->linha->len) return;
    t->cursor = (gsize)(g_utf8_next_char(t->linha->str + t->cursor) - t->linha->str);
}

static void apagar_antes(Term *t)
{
    if (t->cursor == 0) return;
    gsize ini = (gsize)(g_utf8_prev_char(t->linha->str + t->cursor) - t->linha->str);
    g_string_erase(t->linha, (gssize)ini, (gssize)(t->cursor - ini));
    t->cursor = ini;
}

static void apagar_sob(Term *t)
{
    if (t->cursor >= t->linha->len) return;
    gsize fim = (gsize)(g_utf8_next_char(t->linha->str + t->cursor) - t->linha->str);
    g_string_erase(t->linha, (gssize)t->cursor, (gssize)(fim - t->cursor));
}

static void apagar_palavra_antes(Term *t)
{
    gsize fim = t->cursor;
    while (t->cursor > 0 && t->linha->str[t->cursor - 1] == ' ') t->cursor--;
    while (t->cursor > 0 && t->linha->str[t->cursor - 1] != ' ') t->cursor--;
    g_string_erase(t->linha, (gssize)t->cursor, (gssize)(fim - t->cursor));
}

static void definir_linha(Term *t, const char *s)
{
    g_string_assign(t->linha, s ? s : "");
    t->cursor = t->linha->len;
    t->desloc = 0;
}

static void historico_mover(Term *t, int delta)
{
    int n = (int)t->historico->len;
    if (n == 0) return;
    int novo = t->hist_pos;
    if (delta < 0) { /* para trás (mais antigo) */
        if (t->hist_pos == -1) {
            g_free(t->rascunho);
            t->rascunho = g_strdup(t->linha->str);
            novo = n - 1;
        } else if (t->hist_pos > 0) {
            novo = t->hist_pos - 1;
        }
    } else {
        if (t->hist_pos == -1) return;
        novo = t->hist_pos + 1 < n ? t->hist_pos + 1 : -1;
    }
    if (novo == t->hist_pos) return;
    t->hist_pos = novo;
    definir_linha(t, novo == -1 ? t->rascunho : g_ptr_array_index(t->historico, (guint)novo));
}

static void aceitar_linha(Term *t)
{
    char *linha = g_strdup(t->linha->str);
    /* A linha aceita vira UMA linha do histórico (eco), e a região viva é
     * redesenhada logo abaixo, já com a entrada vazia — é por construção
     * que a pergunta enviada nunca continua (nem reaparece) no prompt. */
    GString *eco = g_string_new(NULL);
    if (t->cor) g_string_append(eco, "\033[1m");
    g_string_append_printf(eco, "%s%s", t->prompt, linha);
    if (t->cor) g_string_append(eco, "\033[0m");
    g_string_truncate(t->linha, 0);
    t->cursor = 0;
    t->desloc = 0;
    t->hist_pos = -1;
    g_clear_pointer(&t->rascunho, g_free);
    if (*linha) {
        guint n = t->historico->len;
        if (n == 0 || strcmp(g_ptr_array_index(t->historico, n - 1), linha) != 0)
            g_ptr_array_add(t->historico, g_strdup(linha));
        if (t->historico->len > TERM_HISTORICO_MAX)
            g_ptr_array_remove_index(t->historico, 0);
        term_imprimir(t, eco->str);
    } else {
        redesenhar(t);
    }
    g_string_free(eco, TRUE);
    t->cb(linha, t->dados);
    g_free(linha);
}

static void tratar_csi(Term *t, const char *params, char final)
{
    if (final == '~') {
        int n = atoi(params);
        switch (n) {
        case 1: case 7: t->cursor = 0; break;
        case 4: case 8: t->cursor = t->linha->len; break;
        case 3: apagar_sob(t); break;
        case 200: t->colando = TRUE; break;
        case 201: t->colando = FALSE; break;
        default: break; /* PgUp/PgDn/Insert/F-keys: ignoradas */
        }
        return;
    }
    switch (final) {
    case 'A': historico_mover(t, -1); break;
    case 'B': historico_mover(t, +1); break;
    case 'C': mover_direita(t); break;
    case 'D': mover_esquerda(t); break;
    case 'H': t->cursor = 0; break;
    case 'F': t->cursor = t->linha->len; break;
    default: break;
    }
}

/* Processa um byte. Devolve TRUE se a região precisa ser redesenhada. */
static gboolean processar_byte(Term *t, unsigned char b)
{
    if (t->esc == ESC_ESC) {
        if (b == '[') { t->esc = ESC_CSI; g_string_truncate(t->csi, 0); return FALSE; }
        if (b == 'O') { t->esc = ESC_SS3; return FALSE; }
        t->esc = ESC_NADA;
        if (b == 0x1b) { t->esc = ESC_ESC; return FALSE; }
        if (b == 'b') { /* Alt-b: palavra anterior */
            while (t->cursor > 0 && t->linha->str[t->cursor - 1] == ' ') t->cursor--;
            while (t->cursor > 0 && t->linha->str[t->cursor - 1] != ' ') t->cursor--;
            return TRUE;
        }
        if (b == 'f') { /* Alt-f: próxima palavra */
            while (t->cursor < t->linha->len && t->linha->str[t->cursor] == ' ') t->cursor++;
            while (t->cursor < t->linha->len && t->linha->str[t->cursor] != ' ') t->cursor++;
            return TRUE;
        }
        return FALSE; /* ESC + tecla desconhecida: descarta as duas */
    }
    if (t->esc == ESC_CSI) {
        if (b >= 0x40 && b <= 0x7e) {
            t->esc = ESC_NADA;
            tratar_csi(t, t->csi->str, (char)b);
            return TRUE;
        }
        if (t->csi->len < 32) g_string_append_c(t->csi, (char)b);
        else t->esc = ESC_NADA; /* sequência absurda: abandona */
        return FALSE;
    }
    if (t->esc == ESC_SS3) {
        t->esc = ESC_NADA;
        tratar_csi(t, "", (char)b); /* ESC O A/B/C/D/H/F: mesmas teclas em modo aplicação */
        return TRUE;
    }
    if (b == 0x1b) { t->esc = ESC_ESC; t->utf8_n = t->utf8_falta = 0; return FALSE; }

    if (t->colando) {
        /* texto colado: quebra de linha e tab viram espaço (nunca enviam a
         * linha), outros caracteres de controle são descartados */
        if (b == '\r' || b == '\n' || b == '\t') { inserir(t, " ", 1); return TRUE; }
        if (b < 0x20 || b == 0x7f) return FALSE;
    } else if (b < 0x20 || b == 0x7f) {
        switch (b) {
        case '\r': case '\n': aceitar_linha(t); return FALSE; /* aceitar_linha já redesenhou */
        case 0x7f: case 0x08: apagar_antes(t); return TRUE;
        case 0x01: t->cursor = 0; return TRUE;                       /* Ctrl-A */
        case 0x05: t->cursor = t->linha->len; return TRUE;           /* Ctrl-E */
        case 0x02: mover_esquerda(t); return TRUE;                   /* Ctrl-B */
        case 0x06: mover_direita(t); return TRUE;                    /* Ctrl-F */
        case 0x0b: g_string_truncate(t->linha, t->cursor); return TRUE; /* Ctrl-K */
        case 0x15: g_string_erase(t->linha, 0, (gssize)t->cursor); t->cursor = 0; return TRUE; /* Ctrl-U */
        case 0x17: apagar_palavra_antes(t); return TRUE;             /* Ctrl-W */
        case 0x10: historico_mover(t, -1); return TRUE;              /* Ctrl-P */
        case 0x0e: historico_mover(t, +1); return TRUE;              /* Ctrl-N */
        case 0x0c: term_limpar_tela(t); return FALSE;                /* Ctrl-L */
        case 0x04:                                                   /* Ctrl-D */
            if (t->linha->len == 0) { t->cb(NULL, t->dados); return FALSE; }
            apagar_sob(t);
            return TRUE;
        default: return FALSE;
        }
    }

    /* texto: monta o caractere UTF-8 inteiro antes de inserir */
    if (t->utf8_falta > 0) {
        if ((b & 0xC0) == 0x80) {
            t->utf8_pend[t->utf8_n++] = (char)b;
            if (--t->utf8_falta == 0) {
                if (g_utf8_validate(t->utf8_pend, t->utf8_n, NULL))
                    inserir(t, t->utf8_pend, (gsize)t->utf8_n);
                t->utf8_n = 0;
                return TRUE;
            }
            return FALSE;
        }
        t->utf8_n = t->utf8_falta = 0; /* sequência quebrada: descarta e reinterpreta este byte */
    }
    if (b < 0x80) { char c = (char)b; inserir(t, &c, 1); return TRUE; }
    int falta = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
    if (falta == 0) return FALSE; /* byte inválido como início de sequência */
    t->utf8_pend[0] = (char)b;
    t->utf8_n = 1;
    t->utf8_falta = falta;
    return FALSE;
}

gboolean term_entrada_disponivel(Term *t)
{
    gboolean leu_algo = FALSE;
    for (;;) {
        int disponivel = 0;
        if (ioctl(t->fd_in, FIONREAD, &disponivel) != 0 || disponivel <= 0)
            break;
        char buf[1024];
        ssize_t n = read(t->fd_in, buf, MIN((gsize)disponivel, sizeof buf));
        if (n <= 0) break;
        leu_algo = TRUE;
        gboolean sujo = FALSE;
        for (ssize_t i = 0; i < n; i++)
            sujo = processar_byte(t, (unsigned char)buf[i]) || sujo;
        if (sujo) redesenhar(t);
    }
    /* o laço principal só chama isto quando o fd está "pronto": pronto e
     * sem nenhum byte para ler é fim de entrada (terminal fechado). */
    return leu_algo;
}
