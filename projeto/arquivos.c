/* Arquivos do projeto — ver projeto/arquivos.h. */
#define _GNU_SOURCE
#include "arquivos.h"

#include <errno.h>
#include <gio/gio.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef LIA_COM_UCHARDET
#include <uchardet.h>
#endif

static char *g_raiz;
static char *g_dir_desfazer;

/* ================================================================ raiz */

static const char *const RAIZES_PROIBIDAS[] = {
    "/", "/bin", "/boot", "/dev", "/etc", "/home", "/lib", "/lib32", "/lib64", "/mnt", "/media", "/opt", "/proc",
    "/root", "/run", "/sbin", "/srv", "/sys", "/tmp", "/usr", "/var", NULL,
};

gboolean proj_definir_raiz(const char *dir, char **erro)
{
    if (!dir || !*dir) { *erro = g_strdup("nenhum diretório de projeto informado"); return FALSE; }
    char real[PATH_MAX];
    if (!realpath(dir, real)) { *erro = g_strdup_printf("o diretório do projeto não existe: %s", dir); return FALSE; }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) { *erro = g_strdup_printf("não é um diretório: %s", real); return FALSE; }
    if (st.st_uid != geteuid()) { *erro = g_strdup_printf("o diretório %s não pertence ao usuário atual", real); return FALSE; }
    for (int i = 0; RAIZES_PROIBIDAS[i]; i++)
        if (!strcmp(real, RAIZES_PROIBIDAS[i]) || (g_str_has_prefix(real, "/usr/") || g_str_has_prefix(real, "/etc/") ||
                                                    g_str_has_prefix(real, "/proc/") || g_str_has_prefix(real, "/sys/") ||
                                                    g_str_has_prefix(real, "/dev/") || g_str_has_prefix(real, "/boot/"))) {
            *erro = g_strdup_printf("recusado: %s é um diretório do sistema, não um projeto", real);
            return FALSE;
        }
    struct passwd *pw = getpwuid(geteuid());
    if (pw && pw->pw_dir && !strcmp(real, pw->pw_dir)) {
        *erro = g_strdup_printf("recusado: %s é a pasta pessoal inteira; escolha a pasta de um projeto", real);
        return FALSE;
    }
    g_free(g_raiz);
    g_raiz = g_strdup(real);
    return TRUE;
}

const char *proj_raiz(void) { return g_raiz; }

void proj_definir_dir_desfazer(const char *dir)
{
    g_free(g_dir_desfazer);
    g_dir_desfazer = dir ? g_strdup(dir) : NULL;
}

static const char *dir_desfazer(void)
{
    if (!g_dir_desfazer) g_dir_desfazer = g_build_filename(g_get_home_dir(), ".lia", "projeto", "desfazer", NULL);
    return g_dir_desfazer;
}

static gboolean dentro_da_raiz(const char *abs)
{
    gsize n = strlen(g_raiz);
    return !strcmp(abs, g_raiz) || (!strncmp(abs, g_raiz, n) && abs[n] == '/');
}

char *proj_relativo(const char *abs)
{
    if (!g_raiz || !abs) return g_strdup(abs ? abs : "");
    if (!strcmp(abs, g_raiz)) return g_strdup(".");
    gsize n = strlen(g_raiz);
    if (!strncmp(abs, g_raiz, n) && abs[n] == '/') return g_strdup(abs + n + 1);
    return g_strdup(abs);
}

static gboolean componente_git(const char *rel)
{
    char **p = g_strsplit(rel, "/", -1);
    gboolean tem = FALSE;
    for (int i = 0; p[i]; i++) if (!strcmp(p[i], ".git")) tem = TRUE;
    g_strfreev(p);
    return tem;
}

char *proj_resolver(const char *caminho, gboolean escrita, gboolean deve_existir, char **erro)
{
    if (!g_raiz) { *erro = g_strdup("nenhum projeto definido (LIA_SESSIOND_PROJETO)"); return NULL; }
    if (!caminho || !*caminho) caminho = ".";
    for (const char *q = caminho; *q; q++)
        if ((unsigned char)*q < 0x20) { *erro = g_strdup("caminho com caractere de controle"); return NULL; }
    if (*caminho == '~') { *erro = g_strdup("use caminhos relativos ao projeto (sem '~')"); return NULL; }
    char *abs = g_canonicalize_filename(caminho, g_raiz);
    if (!dentro_da_raiz(abs)) {
        *erro = g_strdup_printf("recusado: '%s' fica fora do projeto (%s)", caminho, g_raiz);
        g_free(abs);
        return NULL;
    }
    char *rel = proj_relativo(abs);
    if (escrita && (!strcmp(abs, g_raiz) || componente_git(rel))) {
        *erro = g_strdup_printf(!strcmp(abs, g_raiz) ? "recusado: a raiz do projeto não pode ser alterada"
                                                     : "recusado: '%s' é estado interno do Git (.git/) — use o Git para isso", rel);
        g_free(abs); g_free(rel);
        return NULL;
    }
    /* links simbólicos: o ancestral existente mais profundo, resolvido,
     * precisa continuar dentro da raiz */
    char *busca = g_strdup(abs);
    struct stat st;
    while (lstat(busca, &st) != 0) {
        char *pai = g_path_get_dirname(busca);
        g_free(busca);
        busca = pai;
    }
    char real[PATH_MAX];
    gboolean final_existe = !strcmp(busca, abs);
    gboolean final_link = final_existe && S_ISLNK(st.st_mode);
    /* para o próprio link numa escrita (apagar/mover o link), confere o pai */
    char *conferir = (final_link && escrita) ? g_path_get_dirname(abs) : g_strdup(busca);
    if (!realpath(conferir, real)) {
        if (final_link && !escrita) { *erro = g_strdup_printf("link simbólico quebrado: %s", rel); }
        else *erro = g_strdup_printf("não foi possível resolver '%s': %s", rel, g_strerror(errno));
        g_free(conferir); g_free(busca); g_free(abs); g_free(rel);
        return NULL;
    }
    g_free(conferir);
    g_free(busca);
    if (!dentro_da_raiz(real)) {
        *erro = g_strdup_printf("recusado: '%s' aponta (por link simbólico) para fora do projeto", rel);
        g_free(abs); g_free(rel);
        return NULL;
    }
    if (deve_existir && !final_existe) {
        *erro = g_strdup_printf("não existe no projeto: %s", rel);
        g_free(abs); g_free(rel);
        return NULL;
    }
    if (final_existe && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        *erro = g_strdup_printf("recusado: '%s' é um arquivo especial (dispositivo, FIFO ou socket)", rel);
        g_free(abs); g_free(rel);
        return NULL;
    }
    g_free(rel);
    return abs;
}

/* ========================================================= codificação */

const char *proj_confianca_texto(ProjConfianca c)
{
    return c == PROJ_CONF_ALTA ? "alta" : c == PROJ_CONF_MEDIA ? "média" : "baixa";
}

static gboolean parece_binario(const guchar *d, gsize n)
{
    gsize amostra = n < 8192 ? n : 8192, controle = 0;
    for (gsize i = 0; i < amostra; i++) {
        guchar c = d[i];
        if (c == 0) return TRUE;
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\b' && c != 0x1b && c != 0x0b) controle++;
    }
    return amostra && controle * 10 > amostra; /* >10% de controle */
}

/* UTF-16/32 sem BOM: padrão de zeros em texto majoritariamente ASCII */
static const char *utf_largo_sem_bom(const guchar *d, gsize n)
{
    gsize amostra = (n < 4096 ? n : 4096) & ~(gsize)3;
    if (amostra < 8) return NULL;
    gsize z[4] = { 0 };
    for (gsize i = 0; i < amostra; i++) if (!d[i]) z[i % 4]++;
    gsize q = amostra / 4;
    if (z[1] > q * 9 / 10 && z[2] > q * 9 / 10 && z[3] > q * 9 / 10 && z[0] < q / 2) return "UTF-32LE";
    if (z[0] > q * 9 / 10 && z[1] > q * 9 / 10 && z[2] > q * 9 / 10 && z[3] < q / 2) return "UTF-32BE";
    gsize pares = amostra / 2, imp = z[1] + z[3], par = z[0] + z[2];
    if (imp > pares * 7 / 10 && par < pares / 10) return "UTF-16LE";
    if (par > pares * 7 / 10 && imp < pares / 10) return "UTF-16BE";
    return NULL;
}

/* agrupa escritas que convivem na mesma língua (japonês mistura Han,
 * Hiragana e Katakana; coreano, Hangul e Han) */
static int grupo_escrita(gunichar c)
{
    GUnicodeScript s = g_unichar_get_script(c);
    switch (s) {
    case G_UNICODE_SCRIPT_HAN: case G_UNICODE_SCRIPT_HIRAGANA: case G_UNICODE_SCRIPT_KATAKANA:
    case G_UNICODE_SCRIPT_HANGUL: case G_UNICODE_SCRIPT_BOPOMOFO:
        return 1000;
    default:
        return (int)s;
    }
}

/* Pontuação de uma decodificação legada: fração das letras não-ASCII cujas
 * letras vizinhas são da mesma escrita. "ação" lido como CP1251 vira
 * "aзгo" (latim e cirílico na mesma palavra: incoerente); lido como
 * CP1252, "ação" (coerente). Caracteres de controle ou não atribuídos
 * desclassificam. */
static double coerencia(const char *u)
{
    gunichar ant = 0;
    int n_nao_ascii = 0, coerentes = 0;
    const char *p = u;
    gunichar prev_letra = 0;
    while (*p) {
        gunichar c = g_utf8_get_char(p);
        const char *prox = g_utf8_next_char(p);
        gunichar seguinte = *prox ? g_utf8_get_char(prox) : 0;
        if (c >= 0x80) {
            GUnicodeType t = g_unichar_type(c);
            if (t == G_UNICODE_CONTROL || t == G_UNICODE_UNASSIGNED || t == G_UNICODE_PRIVATE_USE || t == G_UNICODE_SURROGATE)
                return -1.0;
            if (g_unichar_isalpha(c)) {
                n_nao_ascii++;
                int g = grupo_escrita(c);
                gboolean ok = TRUE;
                if (prev_letra && g_unichar_isalpha(prev_letra) && grupo_escrita(prev_letra) != g) ok = FALSE;
                if (seguinte && g_unichar_isalpha(seguinte) && grupo_escrita(seguinte) != g) ok = FALSE;
                if (ok) coerentes++;
            } else if (!g_unichar_isspace(c) && !g_unichar_ispunct(c) && t != G_UNICODE_CURRENCY_SYMBOL &&
                       t != G_UNICODE_OTHER_SYMBOL && t != G_UNICODE_MATH_SYMBOL && !g_unichar_isdigit(c)) {
                n_nao_ascii++; /* marca/símbolo raro: conta como incoerente */
            }
        }
        prev_letra = g_unichar_isalpha(c) ? c : 0;
        ant = c;
        p = prox;
    }
    (void)ant;
    return n_nao_ascii ? (double)coerentes / n_nao_ascii : 0.0;
}

/* Plausibilidade extra: em escritas com maiúsculas (latina, cirílica,
 * grega) o texto real é quase todo minúsculo; e em línguas latinas as
 * letras acentuadas são minoria (< ~30 % das letras). */
static double plausibilidade(const char *u)
{
    int nao_ascii = 0, minusc = 0, com_caixa = 0, letras = 0, latinas_na = 0;
    for (const char *p = u; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (!g_unichar_isalpha(c)) continue;
        letras++;
        if (c < 0x80) continue;
        nao_ascii++;
        if (g_unichar_get_script(c) == G_UNICODE_SCRIPT_LATIN) latinas_na++;
        if (g_unichar_islower(c)) { minusc++; com_caixa++; }
        else if (g_unichar_isupper(c)) com_caixa++;
    }
    if (!nao_ascii) return 0.0;
    double p = 0.0;
    if (com_caixa > 4) p += 0.5 * ((double)minusc / com_caixa) - 0.25;
    if (letras && latinas_na * 10 > letras * 3) p -= 0.6; /* "Ïðèâåò": latim implausível */
    return p;
}

/* codificações legadas candidatas, em ordem de preferência em empates */
static const char *const LEGADAS[] = {
    "WINDOWS-1252", "ISO-8859-15", "WINDOWS-1250", "WINDOWS-1251", "KOI8-R", "ISO-8859-7", "WINDOWS-1253",
    "WINDOWS-1254", "WINDOWS-1255", "WINDOWS-1256", "WINDOWS-1257", "WINDOWS-1258", "TIS-620",
    "SHIFT_JIS", "EUC-JP", "GB18030", "BIG5", "EUC-KR", NULL,
};

static char *tentar(const guchar *d, gsize n, const char *cod)
{
    gsize lidos = 0, escritos = 0;
    GError *e = NULL;
    char *u = g_convert((const char *)d, (gssize)n, "UTF-8", cod, &lidos, &escritos, &e);
    if (e) { g_error_free(e); g_free(u); return NULL; }
    if (lidos != n) { g_free(u); return NULL; }
    return u;
}

char *proj_detectar_codificacao(const guchar *d, gsize n, gboolean *bom, ProjConfianca *conf)
{
    if (bom) *bom = FALSE;
    if (conf) *conf = PROJ_CONF_ALTA;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) { if (bom) *bom = TRUE; return g_strdup("UTF-8"); }
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xFE && !d[2] && !d[3]) { if (bom) *bom = TRUE; return g_strdup("UTF-32LE"); }
    if (n >= 4 && !d[0] && !d[1] && d[2] == 0xFE && d[3] == 0xFF) { if (bom) *bom = TRUE; return g_strdup("UTF-32BE"); }
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { if (bom) *bom = TRUE; return g_strdup("UTF-16LE"); }
    if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { if (bom) *bom = TRUE; return g_strdup("UTF-16BE"); }
    if (n >= 4 && d[0] == 0x84 && d[1] == 0x31 && d[2] == 0x95 && d[3] == 0x33) { if (bom) *bom = TRUE; return g_strdup("GB18030"); }
    const char *largo = utf_largo_sem_bom(d, n);
    if (largo) { if (conf) *conf = PROJ_CONF_MEDIA; return g_strdup(largo); }
    if (g_utf8_validate((const char *)d, (gssize)n, NULL)) {
        gboolean ascii = TRUE;
        for (gsize i = 0; i < n && ascii; i++) if (d[i] >= 0x80) ascii = FALSE;
        return g_strdup(ascii ? "US-ASCII" : "UTF-8");
    }
    if (parece_binario(d, n)) return g_strdup("binario");
    /* ISO-2022-JP: 7 bits com sequências de escape */
    if (memmem(d, n, "\x1b$B", 3) || memmem(d, n, "\x1b(J", 3)) {
        char *u = tentar(d, n, "ISO-2022-JP");
        if (u) { g_free(u); if (conf) *conf = PROJ_CONF_MEDIA; return g_strdup("ISO-2022-JP"); }
    }
#ifdef LIA_COM_UCHARDET
    {
        /* detector estatístico do Mozilla (uchardet), quando disponível na
         * compilação; a resposta só vale se decodificar o arquivo inteiro */
        uchardet_t ud = uchardet_new();
        if (ud) {
            char *cand = NULL;
            if (uchardet_handle_data(ud, (const char *)d, n) == 0) {
                uchardet_data_end(ud);
                const char *nome = uchardet_get_charset(ud);
                if (nome && *nome && g_ascii_strcasecmp(nome, "ASCII") && g_ascii_strncasecmp(nome, "UTF-", 4))
                    cand = g_ascii_strup(nome, -1);
            }
            uchardet_delete(ud);
            if (cand) {
                if (!strcmp(cand, "SHIFT-JIS")) { g_free(cand); cand = g_strdup("SHIFT_JIS"); }
                if (!strcmp(cand, "GB2312") || !strcmp(cand, "HZ-GB-2312")) { g_free(cand); cand = g_strdup("GB18030"); }
                char *u = tentar(d, n, cand);
                if (u) { g_free(u); if (conf) *conf = PROJ_CONF_MEDIA; return cand; }
                g_free(cand);
            }
        }
    }
#endif
    gsize amostra = n < 65536 ? n : 65536;
    /* não corta no meio de um caractere multibyte: recua até um byte ASCII */
    while (amostra < n && amostra > 0 && d[amostra] >= 0x80) amostra--;
    const char *melhor = NULL;
    double melhor_p = -1.0;
    for (int i = 0; LEGADAS[i]; i++) {
        char *u = tentar(d, amostra, LEGADAS[i]);
        if (!u) continue;
        double p = coerencia(u);
        if (p >= 0) p += plausibilidade(u);
        /* multibyte (CJK) só ganha com coerência alta: decodifica muita coisa por acaso */
        gboolean multibyte = i >= 13;
        if (multibyte) p -= 0.05;
        if (p > melhor_p + 1e-9) { melhor_p = p; melhor = LEGADAS[i]; }
        g_free(u);
    }
    if (!melhor) { if (conf) *conf = PROJ_CONF_BAIXA; return g_strdup("ISO-8859-1"); }
    if (conf) *conf = melhor_p >= 0.95 ? PROJ_CONF_MEDIA : PROJ_CONF_BAIXA;
    return g_strdup(melhor);
}

static gsize tamanho_bom(const char *cod)
{
    if (!g_ascii_strcasecmp(cod, "UTF-8")) return 3;
    if (!g_ascii_strncasecmp(cod, "UTF-16", 6)) return 2;
    if (!g_ascii_strncasecmp(cod, "UTF-32", 6) || !g_ascii_strcasecmp(cod, "GB18030")) return 4;
    return 0;
}

static gboolean tem_bom(const guchar *d, gsize n, const char *cod)
{
    if (!g_ascii_strcasecmp(cod, "UTF-8")) return n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF;
    if (!g_ascii_strcasecmp(cod, "UTF-16LE")) return n >= 2 && d[0] == 0xFF && d[1] == 0xFE;
    if (!g_ascii_strcasecmp(cod, "UTF-16BE")) return n >= 2 && d[0] == 0xFE && d[1] == 0xFF;
    if (!g_ascii_strcasecmp(cod, "UTF-32LE")) return n >= 4 && d[0] == 0xFF && d[1] == 0xFE && !d[2] && !d[3];
    if (!g_ascii_strcasecmp(cod, "UTF-32BE")) return n >= 4 && !d[0] && !d[1] && d[2] == 0xFE && d[3] == 0xFF;
    if (!g_ascii_strcasecmp(cod, "GB18030")) return n >= 4 && d[0] == 0x84 && d[1] == 0x31 && d[2] == 0x95 && d[3] == 0x33;
    return FALSE;
}

static gboolean codificacao_conhecida(const char *cod)
{
    if (!cod || !*cod || strlen(cod) > 64) return FALSE;
    GIConv cd = g_iconv_open("UTF-8", cod);
    if (cd == (GIConv)-1) return FALSE;
    g_iconv_close(cd);
    return TRUE;
}

char *proj_decodificar(const guchar *d, gsize n, const char *forcada, char **usada, gboolean *bom,
                       ProjConfianca *conf, char **erro)
{
    gboolean b = FALSE;
    ProjConfianca cf = PROJ_CONF_ALTA;
    char *cod;
    if (forcada && *forcada) {
        if (!codificacao_conhecida(forcada)) { *erro = g_strdup_printf("codificação desconhecida pelo sistema (iconv): %s", forcada); return NULL; }
        cod = g_ascii_strup(forcada, -1);
        b = tem_bom(d, n, cod);
    } else {
        cod = proj_detectar_codificacao(d, n, &b, &cf);
    }
    if (!strcmp(cod, "binario")) {
        *erro = g_strdup("o arquivo é binário (não é texto em nenhuma codificação reconhecida)");
        if (usada) *usada = cod; else g_free(cod);
        return NULL;
    }
    gsize pular = b ? tamanho_bom(cod) : 0;
    char *u;
    if (!strcmp(cod, "US-ASCII") || !strcmp(cod, "UTF-8")) {
        if (!g_utf8_validate((const char *)d + pular, (gssize)(n - pular), NULL)) {
            *erro = g_strdup_printf("o conteúdo não é %s válido", cod);
            g_free(cod);
            return NULL;
        }
        u = g_strndup((const char *)d + pular, n - pular);
    } else {
        GError *e = NULL;
        gsize lidos = 0;
        u = g_convert((const char *)d + pular, (gssize)(n - pular), "UTF-8", cod, &lidos, NULL, &e);
        if (e) {
            *erro = g_strdup_printf("não foi possível decodificar como %s (byte %" G_GSIZE_FORMAT "): %s", cod, lidos + pular, e->message);
            g_error_free(e);
            g_free(u);
            g_free(cod);
            return NULL;
        }
    }
    if (bom) *bom = b;
    if (conf) *conf = cf;
    if (usada) *usada = cod; else g_free(cod);
    return u;
}

GBytes *proj_codificar(const char *utf8, const char *cod, gboolean bom, char **erro)
{
    if (!cod || !*cod) cod = "UTF-8";
    if (!codificacao_conhecida(cod)) { *erro = g_strdup_printf("codificação desconhecida pelo sistema (iconv): %s", cod); return NULL; }
    GByteArray *out = g_byte_array_new();
    if (bom) {
        static const guchar U8[] = { 0xEF, 0xBB, 0xBF }, U16LE[] = { 0xFF, 0xFE }, U16BE[] = { 0xFE, 0xFF },
                            U32LE[] = { 0xFF, 0xFE, 0, 0 }, U32BE[] = { 0, 0, 0xFE, 0xFF }, GB[] = { 0x84, 0x31, 0x95, 0x33 };
        if (!g_ascii_strcasecmp(cod, "UTF-8")) g_byte_array_append(out, U8, 3);
        else if (!g_ascii_strcasecmp(cod, "UTF-16LE")) g_byte_array_append(out, U16LE, 2);
        else if (!g_ascii_strcasecmp(cod, "UTF-16BE")) g_byte_array_append(out, U16BE, 2);
        else if (!g_ascii_strcasecmp(cod, "UTF-32LE")) g_byte_array_append(out, U32LE, 4);
        else if (!g_ascii_strcasecmp(cod, "UTF-32BE")) g_byte_array_append(out, U32BE, 4);
        else if (!g_ascii_strcasecmp(cod, "GB18030")) g_byte_array_append(out, GB, 4);
    }
    if (!g_ascii_strcasecmp(cod, "UTF-8") || !g_ascii_strcasecmp(cod, "US-ASCII") || !g_ascii_strcasecmp(cod, "ASCII")) {
        if (g_ascii_strcasecmp(cod, "UTF-8"))
            for (const char *p = utf8; *p; p++)
                if ((unsigned char)*p >= 0x80) {
                    *erro = g_strdup_printf("o texto tem caracteres fora de %s (por exemplo, perto de \"%.20s\")", cod, p);
                    g_byte_array_unref(out);
                    return NULL;
                }
        g_byte_array_append(out, (const guchar *)utf8, (guint)strlen(utf8));
    } else {
        GError *e = NULL;
        gsize lidos = 0, escritos = 0;
        char *c = g_convert(utf8, -1, cod, "UTF-8", &lidos, &escritos, &e);
        if (e) {
            *erro = g_strdup_printf("o texto não cabe em %s: %s (perto de \"%.20s\")", cod, e->message, utf8 + lidos);
            g_error_free(e);
            g_free(c);
            g_byte_array_unref(out);
            return NULL;
        }
        g_byte_array_append(out, (const guchar *)c, (guint)escritos);
        g_free(c);
    }
    return g_byte_array_free_to_bytes(out);
}

const char *proj_detectar_eol(const char *t)
{
    gsize crlf = 0, lf = 0, cr = 0;
    for (const char *p = t; *p; p++) {
        if (*p == '\r' && p[1] == '\n') { crlf++; p++; }
        else if (*p == '\n') lf++;
        else if (*p == '\r') cr++;
    }
    int tipos = (crlf > 0) + (lf > 0) + (cr > 0);
    if (tipos == 0) return "nenhuma";
    if (tipos > 1) return "misto";
    return crlf ? "CRLF" : lf ? "LF" : "CR";
}

/* converte para \n internamente (CRLF/CR); "misto"/"nenhuma": intacto */
static char *eol_para_lf(const char *t, const char *eol)
{
    if (!strcmp(eol, "CRLF")) {
        GString *o = g_string_sized_new(strlen(t));
        for (const char *p = t; *p; p++) { if (*p == '\r' && p[1] == '\n') continue; g_string_append_c(o, *p); }
        return g_string_free(o, FALSE);
    }
    if (!strcmp(eol, "CR")) {
        char *c = g_strdup(t);
        for (char *p = c; *p; p++) if (*p == '\r') *p = '\n';
        return c;
    }
    return g_strdup(t);
}

static char *lf_para_eol(const char *t, const char *eol)
{
    if (!strcmp(eol, "CRLF")) {
        GString *o = g_string_sized_new(strlen(t) + strlen(t) / 20);
        for (const char *p = t; *p; p++) { if (*p == '\n' && (p == t || p[-1] != '\r')) g_string_append_c(o, '\r'); g_string_append_c(o, *p); }
        return g_string_free(o, FALSE);
    }
    if (!strcmp(eol, "CR")) {
        char *c = g_strdup(t);
        for (char *p = c; *p; p++) if (*p == '\n') *p = '\r';
        return c;
    }
    return g_strdup(t);
}

/* ============================================================== utilitários */

static char *json_de(JsonBuilder *b)
{
    JsonGenerator *g = json_generator_new();
    JsonNode *r = json_builder_get_root(b);
    json_generator_set_root(g, r);
    char *s = json_generator_to_data(g, NULL);
    json_node_unref(r);
    g_object_unref(g);
    g_object_unref(b);
    return s;
}

static void b_str(JsonBuilder *b, const char *k, const char *v)
{
    json_builder_set_member_name(b, k);
    if (v) json_builder_add_string_value(b, v); else json_builder_add_null_value(b);
}
static void b_int(JsonBuilder *b, const char *k, gint64 v) { json_builder_set_member_name(b, k); json_builder_add_int_value(b, v); }
static void b_bool(JsonBuilder *b, const char *k, gboolean v) { json_builder_set_member_name(b, k); json_builder_add_boolean_value(b, v); }

static const char *p_str(JsonObject *o, const char *k)
{
    if (!o || !json_object_has_member(o, k)) return NULL;
    JsonNode *n = json_object_get_member(o, k);
    return JSON_NODE_TYPE(n) == JSON_NODE_VALUE && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}
static gboolean p_bool(JsonObject *o, const char *k, gboolean padrao)
{
    if (!o || !json_object_has_member(o, k)) return padrao;
    JsonNode *n = json_object_get_member(o, k);
    return JSON_NODE_TYPE(n) == JSON_NODE_VALUE && json_node_get_value_type(n) == G_TYPE_BOOLEAN ? json_node_get_boolean(n) : padrao;
}

static GBytes *ler_bytes(const char *abs, char **erro)
{
    struct stat st;
    if (stat(abs, &st) != 0) { *erro = g_strdup_printf("não foi possível ler: %s", g_strerror(errno)); return NULL; }
    if (S_ISDIR(st.st_mode)) { *erro = g_strdup("é uma pasta, não um arquivo"); return NULL; }
    if (st.st_size > PROJ_MAX_LEITURA_BYTES) {
        *erro = g_strdup_printf("arquivo grande demais para abrir como texto (%lld bytes; limite %d)", (long long)st.st_size, PROJ_MAX_LEITURA_BYTES);
        return NULL;
    }
    char *c = NULL;
    gsize n = 0;
    GError *e = NULL;
    if (!g_file_get_contents(abs, &c, &n, &e)) { *erro = g_strdup(e->message); g_error_free(e); return NULL; }
    return g_bytes_new_take(c, n);
}

static char *sha256_bytes(GBytes *b)
{
    gsize n = 0;
    const guchar *d = g_bytes_get_data(b, &n);
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, d ? d : (const guchar *)"", n);
}

static char *tipo_mime(const char *abs, const guchar *d, gsize n)
{
    gboolean incerto = FALSE;
    char *ct = g_content_type_guess(abs, d, n, &incerto);
    char *mime = g_content_type_get_mime_type(ct);
    g_free(ct);
    return mime ? mime : g_strdup("application/octet-stream");
}

/* Texto de um arquivo, com tudo que é preciso para regravá-lo igual. */
typedef struct {
    char *texto;       /* UTF-8 com \n (se eol uniforme) */
    char *cod;
    gboolean bom;
    ProjConfianca conf;
    char *eol;         /* detectado no texto original */
    char *sha256;
    gsize bytes;
    mode_t modo;
} Texto;

static void texto_liberar(Texto *t)
{
    if (!t) return;
    g_free(t->texto); g_free(t->cod); g_free(t->eol); g_free(t->sha256);
    g_free(t);
}

static Texto *texto_carregar(const char *abs, const char *forcada, char **erro)
{
    GBytes *b = ler_bytes(abs, erro);
    if (!b) return NULL;
    gsize n = 0;
    const guchar *d = g_bytes_get_data(b, &n);
    Texto *t = g_new0(Texto, 1);
    char *bruto = proj_decodificar(d ? d : (const guchar *)"", n, forcada, &t->cod, &t->bom, &t->conf, erro);
    if (!bruto) { g_bytes_unref(b); texto_liberar(t); return NULL; }
    t->eol = g_strdup(proj_detectar_eol(bruto));
    t->texto = eol_para_lf(bruto, t->eol);
    g_free(bruto);
    t->sha256 = sha256_bytes(b);
    t->bytes = n;
    struct stat st;
    t->modo = stat(abs, &st) == 0 ? (st.st_mode & 07777) : 0644;
    g_bytes_unref(b);
    return t;
}

static gint64 contar_linhas(const char *t)
{
    if (!*t) return 0;
    gint64 n = 0;
    for (const char *p = t; *p; p++) if (*p == '\n') n++;
    if (t[strlen(t) - 1] != '\n') n++;
    return n;
}

/* ============================================================== leitura */

static char *hexdump(const guchar *d, gsize n, gsize max)
{
    GString *o = g_string_new(NULL);
    gsize lim = n < max ? n : max;
    for (gsize i = 0; i < lim; i += 16) {
        g_string_append_printf(o, "%08zx  ", i);
        for (gsize k = 0; k < 16; k++) {
            if (i + k < lim) g_string_append_printf(o, "%02x ", d[i + k]); else g_string_append(o, "   ");
            if (k == 7) g_string_append_c(o, ' ');
        }
        g_string_append(o, " |");
        for (gsize k = 0; k < 16 && i + k < lim; k++) g_string_append_c(o, d[i + k] >= 0x20 && d[i + k] < 0x7f ? (char)d[i + k] : '.');
        g_string_append(o, "|\n");
    }
    return g_string_free(o, FALSE);
}

char *proj_ler(const char *caminho, gint64 linha_inicial, gint64 n_linhas, const char *codificacao, char **erro)
{
    char *abs = proj_resolver(caminho, FALSE, TRUE, erro);
    if (!abs) return NULL;
    char *rel = proj_relativo(abs);
    GBytes *b = ler_bytes(abs, erro);
    if (!b) { g_free(abs); g_free(rel); return NULL; }
    gsize n = 0;
    const guchar *d = g_bytes_get_data(b, &n);
    if (!d) d = (const guchar *)"";
    if (linha_inicial < 1) linha_inicial = 1;
    if (n_linhas < 1) n_linhas = 400;
    if (n_linhas > PROJ_MAX_LINHAS_LEITURA) n_linhas = PROJ_MAX_LINHAS_LEITURA;

    JsonBuilder *jb = json_builder_new();
    json_builder_begin_object(jb);
    b_str(jb, "caminho", rel);
    b_int(jb, "bytes", (gint64)n);
    char *sha = sha256_bytes(b);
    b_str(jb, "sha256", sha);
    g_free(sha);
    char *mime = tipo_mime(abs, d, n);
    b_str(jb, "tipo_mime", mime);
    g_free(mime);

    char *cod = NULL, *err2 = NULL;
    gboolean bom = FALSE;
    ProjConfianca conf = PROJ_CONF_ALTA;
    char *bruto = proj_decodificar(d, n, codificacao, &cod, &bom, &conf, &err2);
    if (!bruto) {
        if (cod && !strcmp(cod, "binario")) {
            b_bool(jb, "binario", TRUE);
            char *hx = hexdump(d, n, 512);
            b_str(jb, "hexdump", hx);
            g_free(hx);
            b_bool(jb, "truncado", n > 512);
            g_free(err2);
        } else {
            json_builder_end_object(jb);
            g_object_unref(jb);
            *erro = err2;
            g_free(cod); g_free(abs); g_free(rel); g_bytes_unref(b);
            return NULL;
        }
    } else {
        const char *eol = proj_detectar_eol(bruto);
        char *t = eol_para_lf(bruto, eol);
        g_free(bruto);
        b_bool(jb, "binario", FALSE);
        b_str(jb, "codificacao", cod);
        b_str(jb, "confianca_codificacao", proj_confianca_texto(conf));
        b_bool(jb, "bom", bom);
        b_str(jb, "quebra_de_linha", eol);
        gint64 total = contar_linhas(t);
        b_int(jb, "total_linhas", total);
        /* janela de linhas */
        const char *p = t;
        gint64 l = 1;
        while (l < linha_inicial && *p) { const char *nl = strchr(p, '\n'); if (!nl) { p += strlen(p); break; } p = nl + 1; l++; }
        GString *jan = g_string_new(NULL);
        gint64 lidas = 0;
        while (*p && lidas < n_linhas) {
            const char *nl = strchr(p, '\n');
            gsize tam = nl ? (gsize)(nl - p) : strlen(p);
            g_string_append_len(jan, p, (gssize)tam);
            g_string_append_c(jan, '\n');
            lidas++;
            p = nl ? nl + 1 : p + tam;
        }
        b_int(jb, "linha_inicial", linha_inicial);
        b_int(jb, "linhas_lidas", lidas);
        b_bool(jb, "truncado", linha_inicial + lidas - 1 < total);
        b_str(jb, "conteudo", jan->str);
        g_string_free(jan, TRUE);
        g_free(t);
    }
    json_builder_end_object(jb);
    g_free(cod); g_free(abs); g_free(rel); g_bytes_unref(b);
    return json_de(jb);
}

/* g_ptr_array_sort passa ponteiros para os elementos */
static gint cmp_str_ptr(gconstpointer a, gconstpointer b) { return g_strcmp0(*(char *const *)a, *(char *const *)b); }

typedef struct {
    JsonBuilder *b;
    GPatternSpec *padrao;
    gint64 limite, n;
    gboolean ocultos, truncado;
} Listagem;

static void listar_dir(Listagem *L, const char *abs, gboolean recursivo, int profundidade)
{
    GDir *d = g_dir_open(abs, 0, NULL);
    if (!d) return;
    GPtrArray *nomes = g_ptr_array_new_with_free_func(g_free);
    const char *nm;
    while ((nm = g_dir_read_name(d))) g_ptr_array_add(nomes, g_strdup(nm));
    g_dir_close(d);
    g_ptr_array_sort(nomes, cmp_str_ptr);
    for (guint i = 0; i < nomes->len; i++) {
        const char *nome = g_ptr_array_index(nomes, i);
        if (!strcmp(nome, ".git")) continue;
        if (!L->ocultos && nome[0] == '.') continue;
        char *filho = g_build_filename(abs, nome, NULL);
        struct stat st;
        if (lstat(filho, &st) != 0) { g_free(filho); continue; }
        gboolean dir = S_ISDIR(st.st_mode);
        if (!L->padrao || dir || g_pattern_spec_match_string(L->padrao, nome)) {
            if (L->n >= L->limite) { L->truncado = TRUE; g_free(filho); break; }
            char *rel = proj_relativo(filho);
            json_builder_begin_object(L->b);
            b_str(L->b, "caminho", rel);
            b_str(L->b, "tipo", dir ? "pasta" : S_ISLNK(st.st_mode) ? "link" : S_ISREG(st.st_mode) ? "arquivo" : "especial");
            if (S_ISREG(st.st_mode)) b_int(L->b, "bytes", (gint64)st.st_size);
            json_builder_end_object(L->b);
            g_free(rel);
            L->n++;
        }
        if (dir && recursivo && profundidade < 12) listar_dir(L, filho, TRUE, profundidade + 1);
        g_free(filho);
        if (L->truncado) break;
    }
    g_ptr_array_unref(nomes);
}

char *proj_listar(const char *caminho, gboolean recursivo, const char *padrao, gint64 limite, gboolean ocultos, char **erro)
{
    char *abs = proj_resolver(caminho, FALSE, TRUE, erro);
    if (!abs) return NULL;
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) { *erro = g_strdup("não é uma pasta"); g_free(abs); return NULL; }
    Listagem L = { .b = json_builder_new(), .padrao = padrao && *padrao ? g_pattern_spec_new(padrao) : NULL,
                   .limite = limite > 0 ? limite : 200, .ocultos = ocultos };
    char *rel = proj_relativo(abs);
    json_builder_begin_object(L.b);
    b_str(L.b, "caminho", rel);
    b_str(L.b, "raiz_do_projeto", g_raiz);
    json_builder_set_member_name(L.b, "entradas");
    json_builder_begin_array(L.b);
    listar_dir(&L, abs, recursivo, 0);
    json_builder_end_array(L.b);
    b_int(L.b, "total", L.n);
    b_bool(L.b, "truncado", L.truncado);
    json_builder_end_object(L.b);
    if (L.padrao) g_pattern_spec_free(L.padrao);
    g_free(rel); g_free(abs);
    return json_de(L.b);
}

typedef struct {
    JsonBuilder *b;
    const char *texto;
    char *texto_dobrado;
    GRegex *re;
    gboolean maiusc;
    GPatternSpec *padrao;
    gint64 limite, n, arquivos;
    gboolean truncado;
} Busca;

static void buscar_em(Busca *B, const char *abs)
{
    struct stat st;
    if (lstat(abs, &st) != 0 || S_ISLNK(st.st_mode)) return;
    if (S_ISDIR(st.st_mode)) {
        GDir *d = g_dir_open(abs, 0, NULL);
        if (!d) return;
        GPtrArray *nomes = g_ptr_array_new_with_free_func(g_free);
        const char *nm;
        while ((nm = g_dir_read_name(d))) g_ptr_array_add(nomes, g_strdup(nm));
        g_dir_close(d);
        g_ptr_array_sort(nomes, cmp_str_ptr);
        for (guint i = 0; i < nomes->len && !B->truncado; i++) {
            const char *nome = g_ptr_array_index(nomes, i);
            if (!strcmp(nome, ".git")) continue;
            char *f = g_build_filename(abs, nome, NULL);
            buscar_em(B, f);
            g_free(f);
        }
        g_ptr_array_unref(nomes);
        return;
    }
    if (!S_ISREG(st.st_mode) || st.st_size > 2 * 1024 * 1024) return;
    char *base = g_path_get_basename(abs);
    gboolean casa = !B->padrao || g_pattern_spec_match_string(B->padrao, base);
    g_free(base);
    if (!casa) return;
    char *erro = NULL;
    Texto *t = texto_carregar(abs, NULL, &erro);
    if (!t) { g_free(erro); return; }
    B->arquivos++;
    char *rel = proj_relativo(abs);
    const char *p = t->texto;
    gint64 l = 1;
    while (*p && !B->truncado) {
        const char *nl = strchr(p, '\n');
        gsize tam = nl ? (gsize)(nl - p) : strlen(p);
        char *linha = g_strndup(p, tam);
        gboolean achou;
        if (B->re) achou = g_regex_match(B->re, linha, 0, NULL);
        else if (B->maiusc) achou = strstr(linha, B->texto) != NULL;
        else { char *f = g_utf8_casefold(linha, -1); achou = strstr(f, B->texto_dobrado) != NULL; g_free(f); }
        if (achou) {
            if (B->n >= B->limite) { B->truncado = TRUE; g_free(linha); break; }
            json_builder_begin_object(B->b);
            b_str(B->b, "caminho", rel);
            b_int(B->b, "linha", l);
            if (g_utf8_strlen(linha, -1) > 300) { char *c = g_utf8_substring(linha, 0, 300); b_str(B->b, "texto", c); g_free(c); }
            else b_str(B->b, "texto", linha);
            json_builder_end_object(B->b);
            B->n++;
        }
        g_free(linha);
        p = nl ? nl + 1 : p + tam;
        l++;
    }
    g_free(rel);
    texto_liberar(t);
}

char *proj_buscar(const char *texto, gboolean regex, gboolean maiusc, const char *caminho, const char *padrao,
                  gint64 limite, char **erro)
{
    if (!texto || !*texto) { *erro = g_strdup("informe o texto a procurar"); return NULL; }
    char *abs = proj_resolver(caminho, FALSE, TRUE, erro);
    if (!abs) return NULL;
    Busca B = { .b = json_builder_new(), .texto = texto, .maiusc = maiusc, .limite = limite > 0 ? limite : 50,
                .padrao = padrao && *padrao ? g_pattern_spec_new(padrao) : NULL };
    if (regex) {
        GError *e = NULL;
        B.re = g_regex_new(texto, maiusc ? 0 : G_REGEX_CASELESS, 0, &e);
        if (!B.re) {
            *erro = g_strdup_printf("expressão regular inválida: %s", e->message);
            g_error_free(e);
            g_object_unref(B.b);
            if (B.padrao) g_pattern_spec_free(B.padrao);
            g_free(abs);
            return NULL;
        }
    } else if (!maiusc) {
        B.texto_dobrado = g_utf8_casefold(texto, -1);
    }
    json_builder_begin_object(B.b);
    b_str(B.b, "procurado", texto);
    b_bool(B.b, "regex", regex);
    json_builder_set_member_name(B.b, "ocorrencias");
    json_builder_begin_array(B.b);
    buscar_em(&B, abs);
    json_builder_end_array(B.b);
    b_int(B.b, "total", B.n);
    b_int(B.b, "arquivos_examinados", B.arquivos);
    b_bool(B.b, "truncado", B.truncado);
    json_builder_end_object(B.b);
    if (B.re) g_regex_unref(B.re);
    if (B.padrao) g_pattern_spec_free(B.padrao);
    g_free(B.texto_dobrado);
    g_free(abs);
    return json_de(B.b);
}

char *proj_info(const char *caminho, char **erro)
{
    char *abs = proj_resolver(caminho, FALSE, TRUE, erro);
    if (!abs) return NULL;
    struct stat st;
    if (lstat(abs, &st) != 0) { *erro = g_strdup(g_strerror(errno)); g_free(abs); return NULL; }
    char *rel = proj_relativo(abs);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    b_str(b, "caminho", rel);
    b_str(b, "tipo", S_ISDIR(st.st_mode) ? "pasta" : S_ISLNK(st.st_mode) ? "link" : "arquivo");
    b_int(b, "bytes", (gint64)st.st_size);
    char modo[8];
    g_snprintf(modo, sizeof modo, "%04o", st.st_mode & 07777);
    b_str(b, "permissoes", modo);
    b_bool(b, "executavel", (st.st_mode & S_IXUSR) != 0);
    GDateTime *dt = g_date_time_new_from_unix_local(st.st_mtime);
    char *mt = g_date_time_format_iso8601(dt);
    b_str(b, "modificado_em", mt);
    g_free(mt);
    g_date_time_unref(dt);
    if (S_ISLNK(st.st_mode)) {
        char alvo[PATH_MAX];
        ssize_t k = readlink(abs, alvo, sizeof alvo - 1);
        if (k >= 0) { alvo[k] = 0; b_str(b, "alvo_do_link", alvo); }
    } else if (S_ISREG(st.st_mode)) {
        char *e2 = NULL;
        GBytes *by = st.st_size <= PROJ_MAX_LEITURA_BYTES ? ler_bytes(abs, &e2) : NULL;
        g_free(e2);
        if (by) {
            gsize n = 0;
            const guchar *d = g_bytes_get_data(by, &n);
            char *mime = tipo_mime(abs, d ? d : (const guchar *)"", n);
            b_str(b, "tipo_mime", mime);
            g_free(mime);
            gboolean bom = FALSE;
            ProjConfianca conf;
            char *cod = proj_detectar_codificacao(d ? d : (const guchar *)"", n, &bom, &conf);
            b_bool(b, "binario", !strcmp(cod, "binario"));
            if (strcmp(cod, "binario")) {
                b_str(b, "codificacao", cod);
                b_str(b, "confianca_codificacao", proj_confianca_texto(conf));
                b_bool(b, "bom", bom);
                char *e3 = NULL, *u = proj_decodificar(d ? d : (const guchar *)"", n, cod, NULL, NULL, NULL, &e3);
                if (u) { b_str(b, "quebra_de_linha", proj_detectar_eol(u)); b_int(b, "total_linhas", contar_linhas(u)); g_free(u); }
                g_free(e3);
            }
            g_free(cod);
            char *sha = sha256_bytes(by);
            b_str(b, "sha256", sha);
            g_free(sha);
            g_bytes_unref(by);
        }
    }
    json_builder_end_object(b);
    g_free(rel); g_free(abs);
    return json_de(b);
}

char *proj_ler_texto(const char *caminho, gsize max_bytes, char **sha256, char **erro)
{
    char *abs = proj_resolver(caminho, FALSE, TRUE, erro);
    if (!abs) return NULL;
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) { *erro = g_strdup_printf("não é um arquivo comum: %s", caminho); g_free(abs); return NULL; }
    if ((gsize)st.st_size > max_bytes) { *erro = g_strdup_printf("%s tem %lld bytes (limite %zu)", caminho, (long long)st.st_size, max_bytes); g_free(abs); return NULL; }
    Texto *t = texto_carregar(abs, NULL, erro);
    g_free(abs);
    if (!t) return NULL;
    char *r = g_strdup(t->texto);
    if (sha256) *sha256 = g_strdup(t->sha256);
    texto_liberar(t);
    return r;
}

/* ================================================================== diff */

static char **linhas_de(const char *t, gsize *n)
{
    GPtrArray *a = g_ptr_array_new();
    const char *p = t;
    while (*p) {
        const char *nl = strchr(p, '\n');
        gsize tam = nl ? (gsize)(nl - p) : strlen(p);
        g_ptr_array_add(a, g_strndup(p, tam));
        p = nl ? nl + 1 : p + tam;
    }
    *n = a->len;
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

typedef struct { char op; gsize ia, ib; } Linha; /* ' ' igual, '-' só em a, '+' só em b */

char *proj_diff(const char *rotulo, const char *antes, const char *depois, int ctx, gsize max_bytes)
{
    gsize na, nb;
    char **a = linhas_de(antes ? antes : "", &na), **b = linhas_de(depois ? depois : "", &nb);
    gsize pre = 0;
    while (pre < na && pre < nb && !strcmp(a[pre], b[pre])) pre++;
    gsize suf = 0;
    while (suf < na - pre && suf < nb - pre && !strcmp(a[na - 1 - suf], b[nb - 1 - suf])) suf++;
    gsize ma = na - pre - suf, mb = nb - pre - suf;
    GArray *ops = g_array_new(FALSE, FALSE, sizeof(Linha));
    for (gsize i = 0; i < pre; i++) { Linha l = { ' ', i, i }; g_array_append_val(ops, l); }
    if ((guint64)ma * (guint64)mb <= 4000000ULL && ma && mb) {
        /* LCS no trecho do meio */
        guint32 *dp = g_new0(guint32, (ma + 1) * (mb + 1));
#define DP(i, j) dp[(i) * (mb + 1) + (j)]
        for (gsize i = ma; i-- > 0;)
            for (gsize j = mb; j-- > 0;)
                DP(i, j) = !strcmp(a[pre + i], b[pre + j]) ? DP(i + 1, j + 1) + 1 : MAX(DP(i + 1, j), DP(i, j + 1));
        gsize i = 0, j = 0;
        while (i < ma || j < mb) {
            if (i < ma && j < mb && !strcmp(a[pre + i], b[pre + j])) { Linha l = { ' ', pre + i, pre + j }; g_array_append_val(ops, l); i++; j++; }
            /* empate: remoção antes da inserção, como no diff unificado */
            else if (i < ma && (j == mb || DP(i + 1, j) >= DP(i, j + 1))) { Linha l = { '-', pre + i, pre + j }; g_array_append_val(ops, l); i++; }
            else { Linha l = { '+', pre + i, pre + j }; g_array_append_val(ops, l); j++; }
        }
#undef DP
        g_free(dp);
    } else {
        for (gsize i = 0; i < ma; i++) { Linha l = { '-', pre + i, pre }; g_array_append_val(ops, l); }
        for (gsize j = 0; j < mb; j++) { Linha l = { '+', pre + ma, pre + j }; g_array_append_val(ops, l); }
    }
    for (gsize k = 0; k < suf; k++) { Linha l = { ' ', na - suf + k, nb - suf + k }; g_array_append_val(ops, l); }

    GString *o = g_string_new(NULL);
    g_string_append_printf(o, "--- a/%s\n+++ b/%s\n", rotulo, rotulo);
    guint n = ops->len;
    guint k = 0;
    gboolean cortado = FALSE;
    while (k < n) {
        while (k < n && g_array_index(ops, Linha, k).op == ' ') k++;
        if (k >= n) break;
        guint ini = k >= (guint)ctx ? k - (guint)ctx : 0, fim = k;
        /* estende o bloco enquanto as mudanças estiverem a <= 2*ctx de distância */
        guint ultimo = k;
        for (guint q = k; q < n; q++) {
            if (g_array_index(ops, Linha, q).op != ' ') ultimo = q;
            else if (q - ultimo > (guint)(2 * ctx)) break;
        }
        fim = MIN(n, ultimo + 1 + (guint)ctx);
        gsize sa = 0, sb = 0, la = 0, lb = 0;
        gboolean primeiro = TRUE;
        for (guint q = ini; q < fim; q++) {
            Linha l = g_array_index(ops, Linha, q);
            if (primeiro) { sa = l.ia + 1; sb = l.ib + 1; primeiro = FALSE; }
            if (l.op != '+') la++;
            if (l.op != '-') lb++;
        }
        if (la == 0) sa--;
        if (lb == 0) sb--;
        g_string_append_printf(o, "@@ -%zu,%zu +%zu,%zu @@\n", sa, la, sb, lb);
        for (guint q = ini; q < fim; q++) {
            Linha l = g_array_index(ops, Linha, q);
            const char *txt = l.op == '+' ? b[l.ib] : a[l.ia];
            g_string_append_c(o, l.op);
            g_string_append(o, txt);
            g_string_append_c(o, '\n');
            if (max_bytes && o->len > max_bytes) { cortado = TRUE; break; }
        }
        if (cortado) break;
        k = fim;
    }
    if (cortado) {
        /* corta numa fronteira de caractere UTF-8 */
        gsize corte = max_bytes;
        while (corte > 0 && ((unsigned char)o->str[corte] & 0xC0) == 0x80) corte--;
        g_string_truncate(o, corte);
        g_string_append(o, "\n… (prévia cortada; a alteração completa será aplicada)\n");
    }
    g_array_unref(ops);
    g_strfreev(a);
    g_strfreev(b);
    return g_string_free(o, FALSE);
}

/* ================================================================ escrita */

static const char *const FERRAMENTAS_ESCRITA[] = {
    "arquivo.criar", "arquivo.escrever", "arquivo.editar", "arquivo.anexar", "arquivo.apagar", "arquivo.mover",
    "arquivo.copiar", "arquivo.criar_pasta", "arquivo.converter", "arquivo.permissoes", "arquivo.desfazer", NULL,
};

gboolean proj_ferramenta_de_escrita(const char *f)
{
    return f && g_strv_contains(FERRAMENTAS_ESCRITA, f);
}

typedef enum { EF_ESCREVER, EF_APAGAR, EF_MKDIR, EF_RENOMEAR, EF_CHMOD } TipoEfeito;

typedef struct {
    TipoEfeito tipo;
    char *abs, *rel;
    char *abs_destino, *rel_destino; /* renomear */
    GBytes *bytes;                   /* escrever */
    mode_t modo;                     /* escrever (arquivo novo) / chmod */
    gboolean existia;
    char *resumo;                    /* uma linha legível */
} Efeito;

static void efeito_liberar(gpointer p)
{
    Efeito *e = p;
    g_free(e->abs); g_free(e->rel); g_free(e->abs_destino); g_free(e->rel_destino); g_free(e->resumo);
    if (e->bytes) g_bytes_unref(e->bytes);
    g_free(e);
}

typedef struct {
    GPtrArray *efeitos;
    GString *previa;
    char *operacao_desfeita; /* desfazer: id da operação restaurada */
} Plano;

static void plano_liberar(Plano *p)
{
    if (!p) return;
    g_ptr_array_unref(p->efeitos);
    g_string_free(p->previa, TRUE);
    g_free(p->operacao_desfeita);
    g_free(p);
}

static void previa_cabecalho(Plano *p, const char *linha)
{
    if (p->previa->len) g_string_append_c(p->previa, '\n');
    g_string_append(p->previa, linha);
    g_string_append_c(p->previa, '\n');
}

static gboolean conferir_sha(JsonObject *params, const Texto *t, const char *rel, char **erro)
{
    const char *esperado = p_str(params, "sha256_esperado");
    if (esperado && *esperado && g_ascii_strcasecmp(esperado, t->sha256) != 0) {
        *erro = g_strdup_printf("'%s' mudou desde que foi lido (sha256 diferente do esperado); leia de novo antes de alterar", rel);
        return FALSE;
    }
    return TRUE;
}

static const char *eol_param(JsonObject *params, const char *padrao, char **erro)
{
    const char *e = p_str(params, "quebra_de_linha");
    if (!e) return padrao;
    if (!strcmp(e, "LF") || !strcmp(e, "CRLF") || !strcmp(e, "CR")) return e;
    *erro = g_strdup("quebra_de_linha deve ser LF, CRLF ou CR");
    return NULL;
}

/* novo conteúdo (UTF-8 com \n) -> bytes na codificação/EOL/BOM pedidos */
static GBytes *montar_bytes(const char *texto_lf, const char *cod, const char *eol, gboolean bom, char **erro)
{
    char *t = (!strcmp(eol, "misto") || !strcmp(eol, "nenhuma")) ? g_strdup(texto_lf) : lf_para_eol(texto_lf, eol);
    GBytes *b = proj_codificar(t, cod, bom, erro);
    g_free(t);
    return b;
}

static void adicionar_escrita(Plano *p, const char *abs, const char *rel, const char *antes, const char *depois,
                              GBytes *bytes, gboolean existia, mode_t modo, const char *resumo)
{
    Efeito *e = g_new0(Efeito, 1);
    e->tipo = EF_ESCREVER;
    e->abs = g_strdup(abs);
    e->rel = g_strdup(rel);
    e->bytes = bytes;
    e->existia = existia;
    e->modo = modo;
    e->resumo = g_strdup(resumo);
    g_ptr_array_add(p->efeitos, e);
    previa_cabecalho(p, resumo);
    char *d = proj_diff(rel, antes ? antes : "", depois, 3, PROJ_MAX_PREVIA_BYTES);
    g_string_append(p->previa, d);
    g_free(d);
}

static gboolean e_pasta(const char *abs) { struct stat st; return stat(abs, &st) == 0 && S_ISDIR(st.st_mode); }
static gboolean existe(const char *abs) { struct stat st; return lstat(abs, &st) == 0; }

static gboolean pai_existe(const char *abs, const char *rel, char **erro)
{
    char *pai = g_path_get_dirname(abs);
    gboolean ok = e_pasta(pai);
    g_free(pai);
    if (!ok) *erro = g_strdup_printf("a pasta de '%s' não existe — crie-a antes (arquivo.criar_pasta)", rel);
    return ok;
}

static Plano *planejar(const char *f, JsonObject *params, char **erro);

static Plano *planejar_desfazer(JsonObject *params, char **erro)
{
    const char *id = p_str(params, "operacao_id");
    if (!id || !*id || strchr(id, '/') || strstr(id, "..")) { *erro = g_strdup("informe um operacao_id válido"); return NULL; }
    char *dir = g_build_filename(dir_desfazer(), id, NULL);
    char *man = g_build_filename(dir, "manifesto.json", NULL);
    JsonParser *jp = json_parser_new();
    Plano *p = NULL;
    if (!json_parser_load_from_file(jp, man, NULL) || JSON_NODE_TYPE(json_parser_get_root(jp)) != JSON_NODE_OBJECT) {
        *erro = g_strdup_printf("operação não encontrada para desfazer: %s", id);
        goto fim;
    }
    JsonObject *raiz = json_node_get_object(json_parser_get_root(jp));
    if (json_object_has_member(raiz, "desfeita") && json_object_get_boolean_member(raiz, "desfeita")) {
        *erro = g_strdup_printf("a operação %s já foi desfeita", id);
        goto fim;
    }
    if (g_strcmp0(p_str(raiz, "raiz"), g_raiz) != 0) {
        *erro = g_strdup_printf("a operação %s pertence a outro projeto (%s)", id, p_str(raiz, "raiz") ? p_str(raiz, "raiz") : "?");
        goto fim;
    }
    p = g_new0(Plano, 1);
    p->efeitos = g_ptr_array_new_with_free_func(efeito_liberar);
    p->previa = g_string_new(NULL);
    p->operacao_desfeita = g_strdup(id);
    JsonArray *ent = json_object_get_array_member(raiz, "entradas");
    /* desfaz na ordem inversa */
    for (guint k = json_array_get_length(ent); k-- > 0;) {
        JsonObject *o = json_array_get_object_element(ent, k);
        const char *acao = p_str(o, "acao"), *rel = p_str(o, "caminho"), *rel_dest = p_str(o, "destino");
        const char *backup = p_str(o, "backup");
        gboolean existia = p_bool(o, "existia", FALSE);
        char *abs = proj_resolver(rel, TRUE, FALSE, erro);
        if (!abs) { plano_liberar(p); p = NULL; goto fim; }
        if (!strcmp(acao, "renomear")) {
            char *abs_d = proj_resolver(rel_dest, TRUE, TRUE, erro);
            if (!abs_d) { g_free(abs); plano_liberar(p); p = NULL; goto fim; }
            if (existe(abs)) { *erro = g_strdup_printf("não dá para desfazer: '%s' já existe de novo", rel); g_free(abs); g_free(abs_d); plano_liberar(p); p = NULL; goto fim; }
            Efeito *e = g_new0(Efeito, 1);
            e->tipo = EF_RENOMEAR; e->abs = abs_d; e->rel = proj_relativo(abs_d); e->abs_destino = abs; e->rel_destino = g_strdup(rel);
            e->resumo = g_strdup_printf("mover de volta: %s → %s", rel_dest, rel);
            g_ptr_array_add(p->efeitos, e);
            previa_cabecalho(p, e->resumo);
            continue;
        }
        if (!strcmp(acao, "mkdir")) {
            Efeito *e = g_new0(Efeito, 1);
            e->tipo = EF_APAGAR; e->abs = abs; e->rel = g_strdup(rel); e->existia = TRUE;
            e->resumo = g_strdup_printf("remover a pasta criada: %s (só se estiver vazia)", rel);
            g_ptr_array_add(p->efeitos, e);
            previa_cabecalho(p, e->resumo);
            continue;
        }
        if (!strcmp(acao, "chmod")) {
            Efeito *e = g_new0(Efeito, 1);
            e->tipo = EF_CHMOD; e->abs = abs; e->rel = g_strdup(rel);
            e->modo = (mode_t)json_object_get_int_member(o, "modo_antes");
            e->resumo = g_strdup_printf("restaurar permissões %04o: %s", (unsigned)e->modo, rel);
            g_ptr_array_add(p->efeitos, e);
            previa_cabecalho(p, e->resumo);
            continue;
        }
        /* escrever / apagar: restaura o conteúdo anterior (ou remove o criado) */
        char *atual = NULL;
        if (existe(abs)) { Texto *t = texto_carregar(abs, NULL, erro); if (t) { atual = g_strdup(t->texto); texto_liberar(t); } g_clear_pointer(erro, g_free); }
        if (existia && backup) {
            char *bk = g_build_filename(dir, backup, NULL);
            char *c = NULL; gsize n = 0;
            if (!g_file_get_contents(bk, &c, &n, NULL)) { *erro = g_strdup_printf("cópia de segurança ausente: %s", bk); g_free(bk); g_free(abs); plano_liberar(p); p = NULL; goto fim; }
            g_free(bk);
            GBytes *by = g_bytes_new_take(c, n);
            char *e2 = NULL, *antes_txt = proj_decodificar(g_bytes_get_data(by, NULL), n, NULL, NULL, NULL, NULL, &e2);
            g_free(e2);
            char *resumo = g_strdup_printf("restaurar: %s", rel);
            adicionar_escrita(p, abs, rel, atual ? atual : "", antes_txt ? antes_txt : "(conteúdo binário)", by,
                              existe(abs), (mode_t)(json_object_has_member(o, "modo_antes") ? json_object_get_int_member(o, "modo_antes") : 0644), resumo);
            g_free(resumo); g_free(antes_txt);
            g_free(abs);
        } else if (!existia) {
            Efeito *e = g_new0(Efeito, 1);
            e->tipo = EF_APAGAR; e->abs = abs; e->rel = g_strdup(rel); e->existia = existe(abs);
            e->resumo = g_strdup_printf("remover o arquivo criado: %s", rel);
            g_ptr_array_add(p->efeitos, e);
            previa_cabecalho(p, e->resumo);
            char *d = proj_diff(rel, atual ? atual : "", "", 3, PROJ_MAX_PREVIA_BYTES);
            g_string_append(p->previa, d);
            g_free(d);
        } else {
            g_free(abs);
        }
        g_free(atual);
    }
fim:
    g_object_unref(jp);
    g_free(man);
    g_free(dir);
    return p;
}

static Plano *planejar(const char *f, JsonObject *params, char **erro)
{
    if (!g_raiz) { *erro = g_strdup("nenhum projeto definido (LIA_SESSIOND_PROJETO)"); return NULL; }
    if (!strcmp(f, "arquivo.desfazer")) return planejar_desfazer(params, erro);
    Plano *p = g_new0(Plano, 1);
    p->efeitos = g_ptr_array_new_with_free_func(efeito_liberar);
    p->previa = g_string_new(NULL);
    const char *caminho = p_str(params, "caminho");
    char *abs = NULL, *rel = NULL;
    const char *conteudo = p_str(params, "conteudo");
    if (conteudo && strlen(conteudo) > PROJ_MAX_CONTEUDO_BYTES) {
        *erro = g_strdup_printf("conteúdo grande demais (limite %d bytes)", PROJ_MAX_CONTEUDO_BYTES);
        goto falha;
    }

    if (!strcmp(f, "arquivo.criar")) {
        if (!(abs = proj_resolver(caminho, TRUE, FALSE, erro))) goto falha;
        rel = proj_relativo(abs);
        if (existe(abs)) { *erro = g_strdup_printf("'%s' já existe — para substituir use arquivo.escrever; para mudar um trecho, arquivo.editar", rel); goto falha; }
        if (!pai_existe(abs, rel, erro)) goto falha;
        const char *cod = p_str(params, "codificacao") ? p_str(params, "codificacao") : "UTF-8";
        const char *eol = eol_param(params, "LF", erro);
        if (!eol) goto falha;
        char *norm = eol_para_lf(conteudo ? conteudo : "", "CRLF");
        GBytes *by = montar_bytes(norm, cod, eol, p_bool(params, "bom", FALSE), erro);
        if (!by) { g_free(norm); goto falha; }
        char *resumo = g_strdup_printf("criar %s (%zu bytes, %s, %s%s)", rel, g_bytes_get_size(by), cod, eol, p_bool(params, "bom", FALSE) ? ", com BOM" : "");
        adicionar_escrita(p, abs, rel, NULL, norm, by, FALSE, p_bool(params, "executavel", FALSE) ? 0755 : 0644, resumo);
        g_free(resumo); g_free(norm);
    } else if (!strcmp(f, "arquivo.escrever") || !strcmp(f, "arquivo.editar") || !strcmp(f, "arquivo.anexar") ||
               !strcmp(f, "arquivo.converter")) {
        if (!(abs = proj_resolver(caminho, TRUE, TRUE, erro))) goto falha;
        rel = proj_relativo(abs);
        if (e_pasta(abs)) { *erro = g_strdup_printf("'%s' é uma pasta", rel); goto falha; }
        struct stat stl;
        if (lstat(abs, &stl) == 0 && S_ISLNK(stl.st_mode)) {
            *erro = g_strdup_printf("'%s' é um link simbólico — altere o arquivo para o qual ele aponta", rel);
            goto falha;
        }
        Texto *t = texto_carregar(abs, p_str(params, "codificacao_atual"), erro);
        if (!t) goto falha;
        if (!conferir_sha(params, t, rel, erro)) { texto_liberar(t); goto falha; }
        char *novo = NULL;
        const char *cod = t->cod, *eol = t->eol;
        gboolean bom = t->bom;
        if (!strcmp(f, "arquivo.escrever")) {
            novo = eol_para_lf(conteudo ? conteudo : "", "CRLF");
        } else if (!strcmp(f, "arquivo.anexar")) {
            char *c = eol_para_lf(conteudo ? conteudo : "", "CRLF");
            gboolean precisa_nl = *t->texto && t->texto[strlen(t->texto) - 1] != '\n' && *c;
            novo = g_strconcat(t->texto, precisa_nl ? "\n" : "", c, NULL);
            g_free(c);
        } else if (!strcmp(f, "arquivo.editar")) {
            const char *proc = p_str(params, "procurar"), *subst = p_str(params, "substituir");
            if (!proc || !*proc || !subst) { *erro = g_strdup("arquivo.editar precisa de 'procurar' (não vazio) e 'substituir'"); texto_liberar(t); goto falha; }
            char *pl = eol_para_lf(proc, "CRLF"), *sl = eol_para_lf(subst, "CRLF");
            int n = 0;
            GString *linhas = g_string_new(NULL);
            for (const char *q = strstr(t->texto, pl); q; q = strstr(q + strlen(pl), pl)) {
                n++;
                if (n <= 10) {
                    gint64 l = 1;
                    for (const char *r = t->texto; r < q; r++) if (*r == '\n') l++;
                    g_string_append_printf(linhas, "%s%" G_GINT64_FORMAT, linhas->len ? ", " : "", l);
                }
            }
            gboolean todas = p_bool(params, "todas", FALSE);
            if (n == 0) {
                *erro = g_strdup_printf("o trecho a substituir não foi encontrado em '%s' (confira espaços, tabs e quebras de linha; leia o arquivo antes)", rel);
            } else if (n > 1 && !todas) {
                *erro = g_strdup_printf("o trecho aparece %d vezes em '%s' (linhas %s): inclua mais contexto para torná-lo único, ou use todas=true", n, rel, linhas->str);
            } else {
                GString *o = g_string_new(NULL);
                const char *q = t->texto, *m;
                while ((m = strstr(q, pl))) { g_string_append_len(o, q, m - q); g_string_append(o, sl); q = m + strlen(pl); if (!todas) break; }
                g_string_append(o, q);
                novo = g_string_free(o, FALSE);
            }
            g_string_free(linhas, TRUE);
            g_free(pl); g_free(sl);
            if (!novo) { texto_liberar(t); goto falha; }
        } else { /* converter */
            novo = g_strdup(t->texto);
            if (p_str(params, "codificacao")) cod = p_str(params, "codificacao");
            if (json_object_has_member(params, "bom")) bom = p_bool(params, "bom", bom);
            const char *e2 = eol_param(params, NULL, erro);
            if (*erro) { g_free(novo); texto_liberar(t); goto falha; }
            if (e2) {
                if (!strcmp(t->eol, "misto")) { char *c = g_strdup(novo); g_free(novo); novo = eol_para_lf(c, "CRLF"); g_free(c);
                                                 char *d2 = g_strdup(novo); g_free(novo); novo = eol_para_lf(d2, "CR"); g_free(d2); }
                eol = e2;
            }
            if (!p_str(params, "codificacao") && !e2 && !json_object_has_member(params, "bom")) {
                *erro = g_strdup("arquivo.converter precisa de codificacao, quebra_de_linha ou bom");
                g_free(novo); texto_liberar(t); goto falha;
            }
        }
        if (!strcmp(f, "arquivo.escrever")) {
            if (p_str(params, "codificacao")) cod = p_str(params, "codificacao");
            const char *e2 = eol_param(params, NULL, erro);
            if (*erro) { g_free(novo); texto_liberar(t); goto falha; }
            if (e2) eol = e2;
            else if (!strcmp(eol, "nenhuma")) eol = "LF";
        }
        if (!strcmp(eol, "nenhuma") && strchr(novo, '\n')) eol = "LF";
        GBytes *by = montar_bytes(novo, cod, eol, bom, erro);
        if (!by) { g_free(novo); texto_liberar(t); goto falha; }
        gboolean igual = g_bytes_get_size(by) == t->bytes;
        if (igual) {
            char *sha_novo = sha256_bytes(by);
            igual = !strcmp(sha_novo, t->sha256);
            g_free(sha_novo);
        }
        if (igual) {
            *erro = g_strdup_printf("nada a alterar: o conteúdo de '%s' ficaria idêntico", rel);
            g_bytes_unref(by); g_free(novo); texto_liberar(t); goto falha;
        }
        const char *verbo = !strcmp(f, "arquivo.escrever") ? "substituir o conteúdo de" : !strcmp(f, "arquivo.anexar") ? "acrescentar ao fim de"
                          : !strcmp(f, "arquivo.editar") ? "editar" : "converter";
        char *resumo = g_strdup_printf("%s %s (%zu → %zu bytes, %s%s%s, %s)", verbo, rel, t->bytes, g_bytes_get_size(by),
                                       g_ascii_strcasecmp(cod, t->cod) ? t->cod : "", g_ascii_strcasecmp(cod, t->cod) ? " → " : "", cod, eol);
        adicionar_escrita(p, abs, rel, t->texto, novo, by, TRUE, t->modo, resumo);
        g_free(resumo); g_free(novo);
        texto_liberar(t);
    } else if (!strcmp(f, "arquivo.apagar")) {
        if (!(abs = proj_resolver(caminho, TRUE, TRUE, erro))) goto falha;
        rel = proj_relativo(abs);
        struct stat st;
        lstat(abs, &st);
        Efeito *e = g_new0(Efeito, 1);
        e->tipo = EF_APAGAR; e->abs = g_strdup(abs); e->rel = g_strdup(rel); e->existia = TRUE;
        if (S_ISDIR(st.st_mode)) {
            GDir *d = g_dir_open(abs, 0, NULL);
            gboolean vazia = d && !g_dir_read_name(d);
            if (d) g_dir_close(d);
            if (!vazia) { *erro = g_strdup_printf("a pasta '%s' não está vazia — apague os arquivos um a um (nada é apagado em massa)", rel); efeito_liberar(e); goto falha; }
            e->resumo = g_strdup_printf("apagar a pasta vazia %s", rel);
            previa_cabecalho(p, e->resumo);
        } else if (S_ISLNK(st.st_mode)) {
            e->resumo = g_strdup_printf("apagar o link simbólico %s (o alvo não é tocado)", rel);
            previa_cabecalho(p, e->resumo);
        } else {
            Texto *t = texto_carregar(abs, NULL, erro);
            char *antes = NULL;
            if (t) {
                if (!conferir_sha(params, t, rel, erro)) { texto_liberar(t); efeito_liberar(e); goto falha; }
                antes = g_strdup(t->texto);
                texto_liberar(t);
            } else {
                g_clear_pointer(erro, g_free);
            }
            e->resumo = g_strdup_printf("apagar %s (%lld bytes; uma cópia fica guardada para desfazer)", rel, (long long)st.st_size);
            previa_cabecalho(p, e->resumo);
            if (antes) { char *d = proj_diff(rel, antes, "", 3, PROJ_MAX_PREVIA_BYTES); g_string_append(p->previa, d); g_free(d); }
            else g_string_append(p->previa, "(conteúdo binário)\n");
            g_free(antes);
        }
        g_ptr_array_add(p->efeitos, e);
    } else if (!strcmp(f, "arquivo.mover") || !strcmp(f, "arquivo.copiar")) {
        const char *origem = p_str(params, "origem"), *destino = p_str(params, "destino");
        if (!(abs = proj_resolver(origem, TRUE, TRUE, erro))) goto falha;
        rel = proj_relativo(abs);
        char *abs_d = proj_resolver(destino, TRUE, FALSE, erro);
        if (!abs_d) goto falha;
        if (e_pasta(abs_d)) { char *b = g_path_get_basename(abs); char *x = g_build_filename(abs_d, b, NULL); g_free(b); g_free(abs_d); abs_d = x; }
        char *rel_d = proj_relativo(abs_d);
        if (existe(abs_d)) { *erro = g_strdup_printf("'%s' já existe — nada é sobrescrito; apague-o antes se for o caso", rel_d); g_free(abs_d); g_free(rel_d); goto falha; }
        if (!pai_existe(abs_d, rel_d, erro)) { g_free(abs_d); g_free(rel_d); goto falha; }
        if (!strcmp(f, "arquivo.mover")) {
            Efeito *e = g_new0(Efeito, 1);
            e->tipo = EF_RENOMEAR; e->abs = g_strdup(abs); e->rel = g_strdup(rel); e->abs_destino = abs_d; e->rel_destino = rel_d;
            e->resumo = g_strdup_printf("mover/renomear %s → %s", rel, rel_d);
            previa_cabecalho(p, e->resumo);
            g_ptr_array_add(p->efeitos, e);
        } else {
            if (e_pasta(abs)) { *erro = g_strdup("copiar pastas inteiras não é suportado; copie os arquivos um a um"); g_free(abs_d); g_free(rel_d); goto falha; }
            GBytes *by = ler_bytes(abs, erro);
            if (!by) { g_free(abs_d); g_free(rel_d); goto falha; }
            struct stat st;
            stat(abs, &st);
            char *resumo = g_strdup_printf("copiar %s → %s (%zu bytes)", rel, rel_d, g_bytes_get_size(by));
            char *e2 = NULL, *txt = proj_decodificar(g_bytes_get_data(by, NULL), g_bytes_get_size(by), NULL, NULL, NULL, NULL, &e2);
            g_free(e2);
            adicionar_escrita(p, abs_d, rel_d, NULL, txt ? txt : "(conteúdo binário)", by, FALSE, st.st_mode & 07777, resumo);
            g_free(txt); g_free(resumo); g_free(abs_d); g_free(rel_d);
        }
    } else if (!strcmp(f, "arquivo.criar_pasta")) {
        if (!(abs = proj_resolver(caminho, TRUE, FALSE, erro))) goto falha;
        rel = proj_relativo(abs);
        if (existe(abs)) { *erro = g_strdup_printf("'%s' já existe", rel); goto falha; }
        /* cria cada nível que falta, do mais alto para o mais baixo */
        GPtrArray *faltam = g_ptr_array_new_with_free_func(g_free);
        char *q = g_strdup(abs);
        while (!existe(q)) { g_ptr_array_insert(faltam, 0, g_strdup(q)); char *pai = g_path_get_dirname(q); g_free(q); q = pai; }
        g_free(q);
        for (guint i = 0; i < faltam->len; i++) {
            Efeito *e = g_new0(Efeito, 1);
            e->tipo = EF_MKDIR; e->abs = g_strdup(g_ptr_array_index(faltam, i)); e->rel = proj_relativo(e->abs);
            e->resumo = g_strdup_printf("criar a pasta %s", e->rel);
            previa_cabecalho(p, e->resumo);
            g_ptr_array_add(p->efeitos, e);
        }
        g_ptr_array_unref(faltam);
    } else if (!strcmp(f, "arquivo.permissoes")) {
        if (!(abs = proj_resolver(caminho, TRUE, TRUE, erro))) goto falha;
        rel = proj_relativo(abs);
        struct stat st;
        if (lstat(abs, &st) != 0 || !S_ISREG(st.st_mode)) { *erro = g_strdup("só arquivos comuns têm a permissão de execução alterada aqui"); goto falha; }
        gboolean exec = p_bool(params, "executavel", TRUE);
        mode_t novo = exec ? (st.st_mode & 07777) | ((st.st_mode & 0444) >> 2) : (st.st_mode & 07777) & ~(mode_t)0111;
        if (novo == (st.st_mode & 07777)) { *erro = g_strdup_printf("nada a alterar: '%s' já %s executável", rel, exec ? "é" : "não é"); goto falha; }
        Efeito *e = g_new0(Efeito, 1);
        e->tipo = EF_CHMOD; e->abs = g_strdup(abs); e->rel = g_strdup(rel); e->modo = novo;
        e->resumo = g_strdup_printf("%s %s (permissões %04o → %04o)", exec ? "tornar executável" : "tirar a permissão de execução de", rel,
                                    (unsigned)(st.st_mode & 07777), (unsigned)novo);
        previa_cabecalho(p, e->resumo);
        g_ptr_array_add(p->efeitos, e);
    } else {
        *erro = g_strdup_printf("ferramenta de arquivo desconhecida: %s", f);
        goto falha;
    }
    g_free(abs);
    g_free(rel);
    return p;
falha:
    g_free(abs);
    g_free(rel);
    plano_liberar(p);
    if (!*erro) *erro = g_strdup("operação inválida");
    return NULL;
}

char *proj_previa(const char *ferramenta, JsonObject *params, char **erro)
{
    Plano *p = planejar(ferramenta, params, erro);
    if (!p) return NULL;
    char *s = g_strdup(p->previa->str);
    plano_liberar(p);
    return s;
}

static gboolean copiar_backup(const char *abs, const char *destino, char **erro)
{
    char *c = NULL;
    gsize n = 0;
    GError *e = NULL;
    if (!g_file_get_contents(abs, &c, &n, &e) || !g_file_set_contents_full(destino, c, (gssize)n, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &e)) {
        *erro = g_strdup_printf("não foi possível guardar a cópia de segurança: %s", e ? e->message : "?");
        if (e) g_error_free(e);
        g_free(c);
        return FALSE;
    }
    g_free(c);
    return TRUE;
}

char *proj_aplicar(const char *ferramenta, JsonObject *params, char **erro)
{
    Plano *p = planejar(ferramenta, params, erro);
    if (!p) return NULL;
    /* diretório da operação (para desfazer) */
    GDateTime *agora = g_date_time_new_now_local();
    char *quando = g_date_time_format(agora, "%Y%m%d-%H%M%S");
    char *op_id = g_strdup_printf("%s-%06x", quando, g_random_int_range(0, 0xffffff));
    g_free(quando);
    g_date_time_unref(agora);
    char *op_dir = g_build_filename(dir_desfazer(), op_id, NULL);
    if (g_mkdir_with_parents(op_dir, 0700) != 0) {
        *erro = g_strdup_printf("não foi possível criar o diretório de desfazer %s: %s", op_dir, g_strerror(errno));
        g_free(op_dir); g_free(op_id); plano_liberar(p);
        return NULL;
    }
    JsonBuilder *man = json_builder_new();
    json_builder_begin_object(man);
    b_str(man, "operacao_id", op_id);
    b_str(man, "ferramenta", ferramenta);
    b_str(man, "raiz", g_raiz);
    json_builder_set_member_name(man, "entradas");
    json_builder_begin_array(man);

    JsonBuilder *res = json_builder_new();
    json_builder_begin_object(res);
    b_str(res, "operacao_id", op_id);
    b_str(res, "ferramenta", ferramenta);
    json_builder_set_member_name(res, "operacoes");
    json_builder_begin_array(res);

    gboolean ok = TRUE;
    guint feitos = 0;
    for (guint i = 0; i < p->efeitos->len && ok; i++) {
        Efeito *e = g_ptr_array_index(p->efeitos, i);
        char *backup = NULL;
        struct stat st;
        gboolean existia = lstat(e->abs, &st) == 0;
        mode_t modo_antes = existia ? (st.st_mode & 07777) : 0;
        if (existia && S_ISREG(st.st_mode) && (e->tipo == EF_ESCREVER || e->tipo == EF_APAGAR)) {
            backup = g_strdup_printf("%u-%s", i, strrchr(e->abs, '/') + 1);
            char *bk = g_build_filename(op_dir, backup, NULL);
            ok = copiar_backup(e->abs, bk, erro);
            g_free(bk);
            if (!ok) { g_free(backup); break; }
        }
        GError *ge = NULL;
        const char *acao = "?";
        switch (e->tipo) {
        case EF_ESCREVER: {
            acao = "escrever";
            gsize n = 0;
            const char *d = g_bytes_get_data(e->bytes, &n);
            ok = g_file_set_contents_full(e->abs, d ? d : "", (gssize)n, G_FILE_SET_CONTENTS_CONSISTENT,
                                          existia ? (int)modo_antes : (int)e->modo, &ge);
            break;
        }
        case EF_APAGAR:
            acao = "apagar";
            if (existia && S_ISLNK(st.st_mode)) {
                char alvo[PATH_MAX];
                ssize_t k = readlink(e->abs, alvo, sizeof alvo - 1);
                if (k >= 0) { alvo[k] = 0; json_builder_begin_object(man); b_str(man, "acao", "link_apagado"); b_str(man, "caminho", e->rel); b_str(man, "alvo", alvo); json_builder_end_object(man); }
            }
            ok = !existia || (S_ISDIR(st.st_mode) ? rmdir(e->abs) == 0 : unlink(e->abs) == 0);
            if (!ok) *erro = g_strdup_printf("não foi possível apagar %s: %s", e->rel, g_strerror(errno));
            break;
        case EF_MKDIR:
            acao = "mkdir";
            ok = mkdir(e->abs, 0755) == 0;
            if (!ok) *erro = g_strdup_printf("não foi possível criar a pasta %s: %s", e->rel, g_strerror(errno));
            break;
        case EF_RENOMEAR:
            acao = "renomear";
            ok = !existe(e->abs_destino) && rename(e->abs, e->abs_destino) == 0;
            if (!ok) *erro = g_strdup_printf("não foi possível mover %s → %s: %s", e->rel, e->rel_destino, existe(e->abs_destino) ? "o destino já existe" : g_strerror(errno));
            break;
        case EF_CHMOD:
            acao = "chmod";
            ok = chmod(e->abs, e->modo) == 0;
            if (!ok) *erro = g_strdup_printf("não foi possível mudar as permissões de %s: %s", e->rel, g_strerror(errno));
            break;
        }
        if (ge) { *erro = g_strdup_printf("não foi possível gravar %s: %s", e->rel, ge->message); g_error_free(ge); }
        if (ok) {
            feitos++;
            /* no desfazer, um "apagar" do que a operação criou registra-se como "escrever" sem cópia (não existia) */
            json_builder_begin_object(man);
            b_str(man, "acao", e->tipo == EF_APAGAR && !backup && !S_ISDIR(st.st_mode) ? "apagar" : acao);
            b_str(man, "caminho", e->tipo == EF_RENOMEAR ? e->rel : e->rel);
            if (e->tipo == EF_RENOMEAR) b_str(man, "destino", e->rel_destino);
            b_bool(man, "existia", existia);
            if (backup) b_str(man, "backup", backup);
            if (existia) b_int(man, "modo_antes", modo_antes);
            json_builder_end_object(man);

            json_builder_begin_object(res);
            b_str(res, "acao", acao);
            b_str(res, "caminho", e->rel);
            if (e->tipo == EF_RENOMEAR) b_str(res, "destino", e->rel_destino);
            b_str(res, "resumo", e->resumo);
            if (e->tipo == EF_ESCREVER) b_int(res, "bytes", (gint64)g_bytes_get_size(e->bytes));
            if (backup) { char *bk = g_build_filename(op_dir, backup, NULL); b_str(res, "copia_original", bk); g_free(bk); }
            json_builder_end_object(res);
        }
        g_free(backup);
    }
    json_builder_end_array(man);
    json_builder_end_object(man);
    char *man_txt = json_de(man);
    char *man_arq = g_build_filename(op_dir, "manifesto.json", NULL);
    g_file_set_contents_full(man_arq, man_txt, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
    g_free(man_arq);
    g_free(man_txt);

    /* desfazer bem-sucedido marca a operação original como desfeita */
    if (ok && p->operacao_desfeita) {
        char *orig = g_build_filename(dir_desfazer(), p->operacao_desfeita, "manifesto.json", NULL);
        JsonParser *jp = json_parser_new();
        if (json_parser_load_from_file(jp, orig, NULL)) {
            JsonObject *o = json_node_get_object(json_parser_get_root(jp));
            json_object_set_boolean_member(o, "desfeita", TRUE);
            JsonGenerator *g = json_generator_new();
            json_generator_set_root(g, json_parser_get_root(jp));
            char *txt = json_generator_to_data(g, NULL);
            g_file_set_contents_full(orig, txt, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
            g_free(txt);
            g_object_unref(g);
        }
        g_object_unref(jp);
        g_free(orig);
    }

    json_builder_end_array(res);
    b_int(res, "operacoes_feitas", feitos);
    b_bool(res, "completa", ok);
    if (!ok) b_str(res, "erro", *erro);
    b_str(res, "diff", p->previa->str);
    b_str(res, "diretorio_desfazer", op_dir);
    json_builder_end_object(res);
    char *saida = json_de(res);
    plano_liberar(p);
    g_free(op_dir);
    g_free(op_id);
    if (!ok && feitos == 0) { g_free(saida); return NULL; }
    if (!ok) { g_free(*erro); *erro = NULL; } /* parcial: o JSON diz o que foi feito e o erro */
    return saida;
}
