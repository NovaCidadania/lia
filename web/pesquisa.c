/* lia-sessiond — ferramentas web.pesquisar e web.abrir (ver web/pesquisa.h).
 *
 * Somente leitura e só HTTPS público: cada destino (inclusive cada salto de
 * redirecionamento) é validado — esquema https, sem credenciais na URL,
 * host que não resolve para rede local/privada/multicast. Corpo limitado a
 * 2 MiB, tempo limitado por requisição, nada é gravado em disco, nenhum
 * JavaScript é executado.
 *
 * Modo de teste: LIA_WEB_FIXTURES_PARA_TESTE=<diretório> troca TODA a rede
 * por arquivos desse diretório (ver web_get_fixture). Só para os testes
 * automatizados, que precisam ser determinísticos e rodar sem rede; não
 * desliga nenhuma outra validação além da resolução DNS dos hosts fictícios.
 */
#include "pesquisa.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define WEB_URL_BASE "https://html.duckduckgo.com/html/?q="
#define WEB_MAX_CORPO (2 * 1024 * 1024)
#define WEB_MAX_CONSULTA 512
#define WEB_TIMEOUT_BUSCA_SEG 20
#define WEB_TIMEOUT_PAGINA_SEG 8
#define WEB_MAX_REDIRECIONAMENTOS 3
#define WEB_MAX_TEXTO 120000
#define WEB_MAX_TRECHO 1800
#define WEB_MAX_ABRIR 4

/* ------------------------------------------------------------- texto --- */

static const struct { const char *nome; const char *utf8; } ENTIDADES[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" }, { "nbsp", " " },
    { "aacute", "á" }, { "eacute", "é" }, { "iacute", "í" }, { "oacute", "ó" }, { "uacute", "ú" },
    { "Aacute", "Á" }, { "Eacute", "É" }, { "Iacute", "Í" }, { "Oacute", "Ó" }, { "Uacute", "Ú" },
    { "atilde", "ã" }, { "otilde", "õ" }, { "Atilde", "Ã" }, { "Otilde", "Õ" }, { "ccedil", "ç" }, { "Ccedil", "Ç" },
    { "acirc", "â" }, { "ecirc", "ê" }, { "ocirc", "ô" }, { "Acirc", "Â" }, { "Ecirc", "Ê" }, { "Ocirc", "Ô" },
    { "agrave", "à" }, { "Agrave", "À" }, { "uuml", "ü" }, { "ouml", "ö" }, { "auml", "ä" }, { "ntilde", "ñ" },
    { "ndash", "–" }, { "mdash", "—" }, { "hellip", "…" }, { "laquo", "«" }, { "raquo", "»" },
    { "ldquo", "“" }, { "rdquo", "”" }, { "lsquo", "‘" }, { "rsquo", "’" }, { "middot", "·" },
    { "copy", "©" }, { "reg", "®" }, { "deg", "°" }, { "times", "×" }, { "bull", "•" },
};

static char *decodificar_entidades(const char *s)
{
    GString *o = g_string_new(NULL);
    for (const char *p = s ? s : ""; *p;) {
        if (*p == '&') {
            const char *fim = strchr(p, ';');
            if (fim && fim - p <= 10) {
                char nome[16];
                gsize n = (gsize)(fim - p - 1);
                memcpy(nome, p + 1, n);
                nome[n] = '\0';
                gunichar ch = 0;
                if (nome[0] == '#') {
                    gboolean hex = nome[1] == 'x' || nome[1] == 'X';
                    ch = (gunichar)g_ascii_strtoull(nome + (hex ? 2 : 1), NULL, hex ? 16 : 10);
                    if (ch == 0xa0) ch = ' ';
                }
                if (ch && g_unichar_validate(ch) && ch >= 0x20) {
                    g_string_append_unichar(o, ch);
                    p = fim + 1;
                    continue;
                }
                gboolean achou = FALSE;
                for (gsize i = 0; i < G_N_ELEMENTS(ENTIDADES); i++)
                    if (!strcmp(nome, ENTIDADES[i].nome)) { g_string_append(o, ENTIDADES[i].utf8); achou = TRUE; break; }
                if (achou) { p = fim + 1; continue; }
            }
        }
        g_string_append_c(o, *p++);
    }
    return g_string_free(o, FALSE);
}

static char *regex_trocar(const char *padrao, const char *texto, const char *por)
{
    GRegex *r = g_regex_new(padrao, G_REGEX_OPTIMIZE, 0, NULL);
    if (!r) return g_strdup(texto);
    char *s = g_regex_replace(r, texto, -1, 0, por, 0, NULL);
    g_regex_unref(r);
    return s ? s : g_strdup(texto);
}

/* tags removidas, entidades decodificadas, espaços colapsados numa linha */
static char *html_limpar(const char *s)
{
    if (!s) return g_strdup("");
    char *sem_tags = regex_trocar("(?s)<[^>]*>", s, "");
    char *dec = decodificar_entidades(sem_tags);
    g_free(sem_tags);
    char *valido = g_utf8_make_valid(dec, -1);
    g_free(dec);
    char *um_espaco = regex_trocar("\\s+", valido, " ");
    g_free(valido);
    g_strstrip(um_espaco);
    return um_espaco;
}

/* nome da tag em minúsculas a partir de p (logo depois de '<' ou '</') */
static gsize nome_tag(const char *p, char *dst, gsize cap)
{
    gsize n = 0;
    while (p[n] && (g_ascii_isalnum(p[n]) || p[n] == '-') && n + 1 < cap) { dst[n] = g_ascii_tolower(p[n]); n++; }
    dst[n] = '\0';
    return n;
}

static gboolean tag_na_lista(const char *nome, const char *const *lista)
{
    for (int i = 0; lista[i]; i++) if (!strcmp(nome, lista[i])) return TRUE;
    return FALSE;
}

/* fim de uma tag começando em '<', respeitando aspas nos atributos (um '>'
 * dentro de data-x='…>…' não fecha a tag) */
static const char *fim_da_tag(const char *p)
{
    char aspas = 0;
    for (p++; *p; p++) {
        if (aspas) { if (*p == aspas) aspas = 0; }
        else if (*p == '"' || *p == '\'') aspas = *p;
        else if (*p == '>') return p + 1;
    }
    return p;
}

/* Analisador de HTML para texto, sem regex (robusto a páginas grandes e a
 * atributos com '<'/'>' entre aspas): ignora o conteúdo de script, style,
 * nav, header, footer, aside, form…; blocos viram quebra de linha. */
char *lia_web_html_para_texto(const char *html, gsize tamanho)
{
    static const char *const IGNORAR[] = { "script", "style", "noscript", "svg", "nav", "footer", "header", "form",
                                           "aside", "template", "iframe", "button", "select", "math", "sup", NULL };
    static const char *const BLOCO[] = { "p", "div", "br", "hr", "li", "ul", "ol", "h1", "h2", "h3", "h4", "h5", "h6",
                                         "tr", "td", "th", "table", "section", "article", "blockquote", "dd", "dt", "dl",
                                         "main", "figure", "figcaption", "pre", "title", "caption", NULL };
    char *bruto = g_strndup(html ? html : "", tamanho);
    GString *o = g_string_new(NULL);
    const char *p = bruto;
    while (*p) {
        if (*p != '<') {
            const char *q = strchr(p, '<');
            gsize n = q ? (gsize)(q - p) : strlen(p);
            g_string_append_len(o, p, (gssize)n);
            p += n;
            continue;
        }
        if (g_str_has_prefix(p, "<!--")) {
            const char *f = strstr(p + 4, "-->");
            p = f ? f + 3 : p + strlen(p);
            continue;
        }
        gboolean fecha = p[1] == '/';
        char nome[16];
        gsize n = nome_tag(p + (fecha ? 2 : 1), nome, sizeof nome);
        const char *depois = fim_da_tag(p);
        if (n == 0) { p = depois; continue; } /* "<!DOCTYPE", "<?xml": descarta */
        if (!fecha && tag_na_lista(nome, IGNORAR) && depois[-1] == '>' && depois[-2] != '/') {
            /* pula até o fechamento correspondente (aninhamento do mesmo nome contado) */
            int nivel = 1;
            const char *q = depois;
            while (*q && nivel > 0) {
                const char *lt = strchr(q, '<');
                if (!lt) { q += strlen(q); break; }
                gboolean f2 = lt[1] == '/';
                char nome2[16];
                nome_tag(lt + (f2 ? 2 : 1), nome2, sizeof nome2);
                if (!strcmp(nome2, nome)) nivel += f2 ? -1 : 1;
                q = fim_da_tag(lt);
            }
            p = q;
            g_string_append_c(o, '\n');
            continue;
        }
        if (tag_na_lista(nome, BLOCO)) g_string_append_c(o, '\n');
        else if (o->len && !g_ascii_isspace(o->str[o->len - 1])) g_string_append_c(o, ' ');
        p = depois;
    }
    g_free(bruto);
    char *dec = decodificar_entidades(o->str);
    g_string_free(o, TRUE);
    char *valido = g_utf8_make_valid(dec, -1);
    g_free(dec);
    /* marcas de referência da Wikipédia ("[1]", "[nota 2]") e espaços */
    char *sem_ref = regex_trocar("\\[(\\d+|nota \\d+|carece de fontes\\?|citation needed|editar|edit)\\]", valido, "");
    g_free(valido);
    /* espaços horizontais apenas: "\\v" no PCRE é espaço VERTICAL (inclui \\n) */
    char *espacos = regex_trocar("[ \\t\\r\\f\\x0b\\xa0]+", sem_ref, " ");
    g_free(sem_ref);
    char *linhas = regex_trocar(" *\\n[ \\n]*", espacos, "\n");
    g_free(espacos);
    /* pontuação colada por tags inline: "linguagem , funcional" */
    char *pont = regex_trocar(" ([,.;:!?)])", linhas, "\\1");
    g_free(linhas);
    g_strstrip(pont);
    if (strlen(pont) > WEB_MAX_TEXTO) {
        const char *corte = g_utf8_find_prev_char(pont, pont + WEB_MAX_TEXTO);
        if (corte) pont[corte - pont] = '\0';
    }
    return pont;
}

static char *primeiro_grupo(const char *padrao, const char *texto)
{
    GRegex *r = g_regex_new(padrao, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
    if (!r) return NULL;
    GMatchInfo *mi = NULL;
    char *g = NULL;
    if (g_regex_match(r, texto, 0, &mi)) g = g_match_info_fetch(mi, 1);
    g_match_info_free(mi);
    g_regex_unref(r);
    return g;
}

char *lia_web_html_titulo(const char *html)
{
    if (!html) return NULL;
    char *t = primeiro_grupo("<meta[^>]+property=[\"']og:title[\"'][^>]+content=[\"']([^\"']+)[\"']", html);
    if (!t) t = primeiro_grupo("<title[^>]*>(.*?)</title>", html);
    if (!t) return NULL;
    char *limpo = html_limpar(t);
    g_free(t);
    if (!*limpo || g_utf8_strlen(limpo, -1) > 300) { g_free(limpo); return NULL; }
    return limpo;
}

char *lia_web_html_data(const char *html)
{
    if (!html) return NULL;
    static const char *const PADROES[] = {
        "<meta[^>]+(?:property|name|itemprop)=[\"'](?:article:published_time|og:published_time|datePublished|date|dc\\.date|pubdate)[\"'][^>]+content=[\"']([^\"']+)[\"']",
        "<meta[^>]+content=[\"']([^\"']+)[\"'][^>]+(?:property|name|itemprop)=[\"'](?:article:published_time|datePublished|date)[\"']",
        "\"datePublished\"\\s*:\\s*\"([^\"]+)\"",
        "<meta[^>]+(?:property|name|itemprop)=[\"'](?:article:modified_time|og:updated_time|dateModified|last-modified)[\"'][^>]+content=[\"']([^\"']+)[\"']",
        "\"dateModified\"\\s*:\\s*\"([^\"]+)\"",
        "<time[^>]+datetime=[\"']([^\"']+)[\"']",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(PADROES); i++) {
        char *bruto = primeiro_grupo(PADROES[i], html);
        if (!bruto) continue;
        char *data = primeiro_grupo("((?:19|20)\\d\\d-[01]\\d-[0-3]\\d)", bruto);
        g_free(bruto);
        if (data) return data;
    }
    return NULL;
}

/* ------------------------------------------------------- relevância --- */

static gboolean palavra_vazia(const char *p)
{
    static const char *const IGNORAR[] = {
        "qual", "quais", "que", "quem", "uma", "umas", "uns", "dos", "das", "para", "por", "com", "sem", "de", "do", "da",
        "em", "no", "na", "nos", "nas", "e", "o", "a", "os", "as", "hoje", "agora", "atual", "atualmente", "me", "conte",
        "fale", "fala", "diga", "explique", "descreva", "sobre", "detalhe", "detalhes", "detalhada", "detalhado",
        "favor", "pesquise", "pesquisa", "busque", "procure", "internet", "web", "como", "quando", "onde", "porque",
        "pois", "mais", "muito", "isso", "esse", "essa", "este", "esta", "ele", "ela", "seu", "sua", "the", "and",
        "for", "with", "what", "how", "voce", "lia", "pode", "poderia", "quero", "saber", "gostaria", "dizer", "mostre",
        "liste", "listar", "resuma", "resumo", "breve", "completo", "completa", "forma", "maneira", "tudo", "foi",
        "era", "ser", "sao", "tem", "ter", "entre", "pelo", "pela", "aos", "num", "numa", NULL };
    for (guint i = 0; IGNORAR[i]; i++) if (!strcmp(p, IGNORAR[i])) return TRUE;
    return strlen(p) < 3;
}

static char *normalizar_busca(const char *s)
{
    char *nfd = g_utf8_normalize(s ? s : "", -1, G_NORMALIZE_NFD);
    GString *out = g_string_new(" ");
    for (const char *p = nfd ? nfd : ""; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_ismark(ch)) continue;
        if (ch < 128 && g_ascii_isalnum((char)ch)) g_string_append_c(out, g_ascii_tolower((char)ch));
        else if (out->str[out->len - 1] != ' ') g_string_append_c(out, ' ');
    }
    g_free(nfd);
    if (out->str[out->len - 1] != ' ') g_string_append_c(out, ' ');
    return g_string_free(out, FALSE);
}

char **lia_web_termos(const char *texto)
{
    char *n = normalizar_busca(texto);
    char **partes = g_strsplit(n, " ", -1);
    g_free(n);
    GPtrArray *a = g_ptr_array_new();
    for (guint i = 0; partes[i]; i++) {
        char *p = partes[i];
        if (!*p || palavra_vazia(p)) continue;
        gsize len = strlen(p);
        if (len > 4 && g_str_has_suffix(p, "oes")) { p[len - 3] = '\0'; }
        else if (len > 3 && p[len - 1] == 's') p[len - 1] = '\0';
        gboolean repetido = FALSE;
        for (guint k = 0; k < a->len; k++) if (!strcmp(g_ptr_array_index(a, k), p)) repetido = TRUE;
        if (!repetido) g_ptr_array_add(a, g_strdup(p));
    }
    g_strfreev(partes);
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

/* quantos termos (distintos) aparecem em 'texto_normalizado'; termos longos
 * casam pelo radical de 5 letras ("evolucao" ~ "evoluiu") */
static int termos_presentes(char **termos, const char *texto_normalizado)
{
    int n = 0;
    for (guint i = 0; termos && termos[i]; i++) {
        const char *t = termos[i];
        char radical[8];
        gsize len = strlen(t);
        if (len >= 6) { memcpy(radical, t, 5); radical[5] = '\0'; }
        else g_snprintf(radical, sizeof radical, " %s", t);
        if (strstr(texto_normalizado, radical)) n++;
    }
    return n;
}

static int relevancia_resultado(char **termos, const char *titulo, const char *resumo)
{
    char *t = normalizar_busca(titulo);
    char *s = normalizar_busca(resumo);
    int pontos = termos_presentes(termos, t) * 3 + termos_presentes(termos, s) * 2;
    int numeros = 0;
    for (const char *p = s; *p; p++)
        if (g_ascii_isdigit(*p) && (p == s || !g_ascii_isdigit(p[-1]))) numeros++;
    pontos += MIN(numeros, 3);
    g_free(t); g_free(s);
    return pontos;
}

/* avisos de manutenção, cookies, assinatura, navegação: não são conteúdo */
static gboolean paragrafo_boilerplate(const char *p)
{
    static const char *const SINAIS[] = {
        "utilize fontes", "ajude a inserir", "encontre fontes", "necessitam de formata", "esta página cita fontes",
        "carece de fontes", "cookie", "javascript", "assine", "inscreva", "newsletter", "todos os direitos reservados",
        "política de privacidade", "politica de privacidade", "termos de uso", "faça login", "faca login",
        "this article", "needs additional citations", "you can help", "sign up", "subscribe", "accept all",
        "isFolder", "function(", "=>", "{\"", NULL };
    char *min = g_utf8_strdown(p, -1);
    gboolean achou = FALSE;
    for (int i = 0; SINAIS[i] && !achou; i++) {
        char *s = g_utf8_strdown(SINAIS[i], -1);
        achou = strstr(min, s) != NULL;
        g_free(s);
    }
    g_free(min);
    return achou;
}

/* parágrafo de prosa: comprido, com frases, não uma barra de menu */
static gboolean paragrafo_util(const char *p)
{
    glong n = g_utf8_strlen(p, -1);
    if (n < 80 || paragrafo_boilerplate(p)) return FALSE;
    int palavras = 0, separadores = 0;
    gboolean em_palavra = FALSE;
    for (const char *q = p; *q; q++) {
        if (*q == '|' || *q == '>') separadores++;
        if (g_ascii_isspace(*q)) em_palavra = FALSE;
        else if (!em_palavra) { em_palavra = TRUE; palavras++; }
    }
    if (palavras < 12 || separadores > palavras / 4) return FALSE;
    return strchr(p, '.') || strchr(p, ',') || strchr(p, ';');
}

typedef struct { const char *texto; int pontos; gboolean escolhido; } Paragrafo;

char *lia_web_trecho_relevante(const char *texto, const char *consulta, gsize max_caracteres)
{
    if (!texto || !*texto) return g_strdup("");
    char **linhas = g_strsplit(texto, "\n", -1);
    char **termos = lia_web_termos(consulta);
    GArray *ps = g_array_new(FALSE, FALSE, sizeof(Paragrafo));
    for (guint i = 0; linhas[i]; i++) {
        g_strstrip(linhas[i]);
        if (!paragrafo_util(linhas[i])) continue;
        char *norm = normalizar_busca(linhas[i]);
        Paragrafo p = { linhas[i], termos_presentes(termos, norm) * 10, FALSE };
        g_free(norm);
        g_array_append_val(ps, p);
    }
    GString *o = g_string_new(NULL);
    if (ps->len == 0) {
        /* nada com cara de prosa: o começo do texto, cortado em palavra */
        char *um = regex_trocar("\\s+", texto, " ");
        g_strstrip(um);
        if (g_utf8_strlen(um, -1) > (glong)max_caracteres) {
            char *corte = g_utf8_substring(um, 0, (glong)max_caracteres);
            char *esp = strrchr(corte, ' ');
            if (esp) *esp = '\0';
            g_string_append_printf(o, "%s…", corte);
            g_free(corte);
        } else g_string_append(o, um);
        g_free(um);
    } else {
        /* abertura (o primeiro parágrafo de prosa que fala do assunto —
         * normalmente a definição) sempre entra; depois os parágrafos com
         * mais termos da pergunta */
        guint abertura = 0;
        for (guint i = 0; i < ps->len; i++) if (g_array_index(ps, Paragrafo, i).pontos > 0) { abertura = i; break; }
        g_array_index(ps, Paragrafo, abertura).pontos += 15;
        gsize usado = 0;
        for (;;) {
            int melhor = -1;
            for (guint i = 0; i < ps->len; i++) {
                Paragrafo *p = &g_array_index(ps, Paragrafo, i);
                if (p->escolhido || (i != abertura && p->pontos == 0)) continue;
                if (melhor < 0 || p->pontos > g_array_index(ps, Paragrafo, melhor).pontos) melhor = (int)i;
            }
            if (melhor < 0) break;
            Paragrafo *p = &g_array_index(ps, Paragrafo, melhor);
            gsize n = (gsize)g_utf8_strlen(p->texto, -1);
            if (usado > 0 && usado + n > max_caracteres) { p->escolhido = TRUE; p->pontos = -1; continue; }
            p->escolhido = TRUE;
            usado += n;
            if (usado >= max_caracteres) break;
        }
        for (guint i = 0; i < ps->len; i++) {
            Paragrafo *p = &g_array_index(ps, Paragrafo, i);
            if (!p->escolhido || p->pontos < 0) continue;
            if (o->len) g_string_append_c(o, '\n');
            if ((gsize)g_utf8_strlen(p->texto, -1) > max_caracteres) {
                /* parágrafo enorme: corta na última frase que cabe */
                char *corte = g_utf8_substring(p->texto, 0, (glong)max_caracteres);
                char *ponto = g_strrstr(corte, ". ");
                if (ponto && ponto - corte > (glong)max_caracteres / 3) ponto[1] = '\0';
                g_string_append(o, corte);
                g_free(corte);
            } else {
                g_string_append(o, p->texto);
            }
        }
    }
    g_array_free(ps, TRUE);
    g_strfreev(termos);
    g_strfreev(linhas);
    return g_string_free(o, FALSE);
}

/* ------------------------------------------------------- transporte --- */

static const char *dir_fixtures(void)
{
    const char *d = g_getenv("LIA_WEB_FIXTURES_PARA_TESTE");
    return d && *d ? d : NULL;
}

static gboolean host_publico(const char *host, char **erro_saida)
{
    if (!host || !*host) { if (erro_saida) *erro_saida = g_strdup("URL sem host"); return FALSE; }
    if (dir_fixtures()) return TRUE; /* hosts fictícios dos testes: não há DNS a consultar */
    GResolver *r = g_resolver_get_default(); GError *erro = NULL;
    GList *enderecos = g_resolver_lookup_by_name(r, host, NULL, &erro);
    g_object_unref(r);
    if (!enderecos) {
        if (erro_saida) *erro_saida = g_strdup_printf("host não resolveu: %s", erro ? erro->message : "erro desconhecido");
        if (erro) g_error_free(erro);
        return FALSE;
    }
    gboolean ok = TRUE;
    for (GList *it = enderecos; it; it = it->next) {
        GInetAddress *a = it->data;
        if (g_inet_address_get_is_loopback(a) || g_inet_address_get_is_link_local(a) ||
            g_inet_address_get_is_site_local(a) || g_inet_address_get_is_multicast(a) ||
            g_inet_address_get_is_any(a)) { ok = FALSE; break; }
    }
    g_resolver_free_addresses(enderecos);
    if (!ok && erro_saida) *erro_saida = g_strdup("destino resolve para rede local, privada ou multicast — recusado");
    return ok;
}

static char *url_publica(const char *url, char **erro_saida)
{
    GError *erro = NULL;
    GUri *u = url ? g_uri_parse(url, G_URI_FLAGS_NONE, &erro) : NULL;
    if (!u) { if (erro_saida) *erro_saida = g_strdup_printf("URL inválida: %s", erro ? erro->message : "vazia"); if (erro) g_error_free(erro); return NULL; }
    const char *scheme = g_uri_get_scheme(u), *host = g_uri_get_host(u);
    gboolean ok = scheme && g_ascii_strcasecmp(scheme, "https") == 0 && host &&
                  !g_uri_get_userinfo(u) && g_uri_get_port(u) != 80;
    if (!ok) {
        if (erro_saida) *erro_saida = g_strdup("somente URLs HTTPS públicas, sem credenciais, são aceitas");
        g_uri_unref(u); return NULL;
    }
    if (!host_publico(host, erro_saida)) { g_uri_unref(u); return NULL; }
    char *normalizada = g_uri_to_string(u); g_uri_unref(u); return normalizada;
}

/* nome do arquivo de fixture de uma URL: busca -> "busca", câmbio ->
 * "cambio"; o resto, host+caminho com todo caractere não alfanumérico
 * trocado por '_' (ex.: https://exemplo.org/a/b -> "exemplo_org_a_b"). */
static char *fixture_nome(const char *url)
{
    GUri *u = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    const char *host = u ? g_uri_get_host(u) : "";
    char *nome;
    if (host && strstr(host, "duckduckgo.com")) nome = g_strdup("busca");
    else if (host && (strstr(host, "frankfurter") || strstr(host, "er-api.com"))) nome = g_strdup("cambio");
    else {
        nome = g_strdup_printf("%s%s", host ? host : "", u ? g_uri_get_path(u) : "");
        for (char *p = nome; *p; p++) if (!g_ascii_isalnum(*p)) *p = '_';
        while (*nome && nome[strlen(nome) - 1] == '_') nome[strlen(nome) - 1] = '\0';
    }
    if (u) g_uri_unref(u);
    return nome;
}

static char *fixture_ler(const char *nome, const char *sufixo)
{
    char *arq = g_strdup_printf("%s/%s%s", dir_fixtures(), nome, sufixo);
    char *conteudo = NULL;
    g_file_get_contents(arq, &conteudo, NULL, NULL);
    g_free(arq);
    if (conteudo) g_strstrip(conteudo);
    return conteudo;
}

/* Arquivos de uma fixture <nome>: <nome> (corpo), <nome>.status (HTTP,
 * padrão 200), <nome>.location (redirecionamento), <nome>.erro (falha de
 * transporte, ex.: timeout), <nome>.atraso (milissegundos de espera). */
static GBytes *web_get_fixture(const char *url, guint *status, char **location, char **erro_saida)
{
    char *nome = fixture_nome(url);
    char *atraso = fixture_ler(nome, ".atraso");
    if (atraso) { g_usleep((gulong)g_ascii_strtoull(atraso, NULL, 10) * 1000); g_free(atraso); }
    char *erro = fixture_ler(nome, ".erro");
    if (erro) {
        if (erro_saida) *erro_saida = g_strdup_printf("requisição HTTPS falhou: %s", erro);
        g_free(erro); g_free(nome);
        return NULL;
    }
    char *st = fixture_ler(nome, ".status");
    *status = st ? (guint)g_ascii_strtoull(st, NULL, 10) : 200;
    g_free(st);
    *location = fixture_ler(nome, ".location");
    char *arq = g_strdup_printf("%s/%s", dir_fixtures(), nome);
    char *corpo = NULL; gsize n = 0;
    if (!g_file_get_contents(arq, &corpo, &n, NULL)) {
        if (!*location && *status == 200) *status = 404;
        corpo = g_strdup(""); n = 0;
    }
    g_free(arq); g_free(nome);
    return g_bytes_new_take(corpo, n);
}

/* GET de uma URL JÁ validada, seguindo até 3 redirecionamentos — cada
 * destino é validado de novo por url_publica() antes de ser pedido. */
static GBytes *web_get(const char *url_validada, guint timeout_seg, guint *status_saida, char **url_final, char **erro_saida)
{
    char *atual = g_strdup(url_validada);
    for (int salto = 0; salto <= WEB_MAX_REDIRECIONAMENTOS; salto++) {
        guint status = 0;
        char *location = NULL;
        GBytes *bytes = NULL;
        if (dir_fixtures()) {
            bytes = web_get_fixture(atual, &status, &location, erro_saida);
        } else {
            SoupSession *sessao = soup_session_new();
            g_object_set(sessao, "timeout", timeout_seg, NULL);
            SoupMessage *msg = soup_message_new("GET", atual);
            if (!msg) {
                g_object_unref(sessao);
                if (erro_saida) *erro_saida = g_strdup("requisição HTTPS inválida");
                g_free(atual);
                return NULL;
            }
            soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
            SoupMessageHeaders *h = soup_message_get_request_headers(msg);
            soup_message_headers_append(h, "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) Lia/1.0 (pesquisa local)");
            soup_message_headers_append(h, "Accept-Language", "pt-BR,pt;q=0.9,en;q=0.7");
            GError *erro = NULL;
            bytes = soup_session_send_and_read(sessao, msg, NULL, &erro);
            status = soup_message_get_status(msg);
            const char *loc = soup_message_headers_get_one(soup_message_get_response_headers(msg), "Location");
            location = loc ? g_strdup(loc) : NULL;
            if (!bytes && erro_saida)
                *erro_saida = g_strdup_printf("requisição HTTPS falhou: %s", erro ? erro->message : "sem resposta");
            if (erro) g_error_free(erro);
            g_object_unref(msg);
            g_object_unref(sessao);
        }
        if (!bytes) { g_free(location); g_free(atual); return NULL; }
        if (status >= 300 && status < 400 && location) {
            g_bytes_unref(bytes);
            char *absoluta = g_uri_resolve_relative(atual, location, G_URI_FLAGS_NONE, NULL);
            g_free(location);
            if (salto == WEB_MAX_REDIRECIONAMENTOS || !absoluta) {
                if (erro_saida) *erro_saida = g_strdup("redirecionamentos demais (máximo 3)");
                g_free(absoluta); g_free(atual);
                return NULL;
            }
            char *erro_destino = NULL;
            char *validada = url_publica(absoluta, &erro_destino);
            g_free(absoluta);
            if (!validada) {
                if (erro_saida) *erro_saida = g_strdup_printf("redirecionamento recusado: %s", erro_destino ? erro_destino : "destino inválido");
                g_free(erro_destino); g_free(atual);
                return NULL;
            }
            g_free(atual);
            atual = validada;
            continue;
        }
        g_free(location);
        if (status_saida) *status_saida = status;
        if (url_final) *url_final = g_strdup(atual);
        g_free(atual);
        return bytes;
    }
    g_free(atual);
    return NULL;
}

static char *json_para_texto(JsonBuilder *b)
{
    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *g = json_generator_new();
    json_generator_set_root(g, root);
    char *s = json_generator_to_data(g, NULL);
    json_node_unref(root);
    g_object_unref(g);
    return s;
}

static void add_str(JsonBuilder *b, const char *nome, const char *valor)
{
    json_builder_set_member_name(b, nome);
    char *v = g_utf8_make_valid(valor ? valor : "", -1);
    json_builder_add_string_value(b, v);
    g_free(v);
}

/* --------------------------------------------------------- web.abrir --- */

typedef struct {
    char *url, *url_final, *html, *erro;
    gsize tamanho;
} Pagina;

static void pagina_baixar(Pagina *p, guint timeout_seg)
{
    char *segura = url_publica(p->url, &p->erro);
    if (!segura) return;
    guint status = 0;
    GBytes *bytes = web_get(segura, timeout_seg, &status, &p->url_final, &p->erro);
    g_free(segura);
    if (!bytes) { if (!p->erro) p->erro = g_strdup("sem resposta"); return; }
    gsize n = 0;
    const char *corpo = g_bytes_get_data(bytes, &n);
    if (status < 200 || status >= 300) p->erro = g_strdup_printf("a página respondeu HTTP %u", status);
    else if (n > WEB_MAX_CORPO) p->erro = g_strdup("a página excedeu 2 MiB");
    else { p->html = g_strndup(corpo, n); p->tamanho = n; }
    g_bytes_unref(bytes);
}

static void pagina_liberar(Pagina *p)
{
    g_free(p->url); g_free(p->url_final); g_free(p->html); g_free(p->erro);
}

char *lia_web_abrir(const char *url, char **erro_saida)
{
    if (erro_saida) *erro_saida = NULL;
    Pagina p = { .url = g_strdup(url) };
    pagina_baixar(&p, WEB_TIMEOUT_BUSCA_SEG);
    if (!p.html) {
        if (erro_saida) *erro_saida = g_strdup(p.erro ? p.erro : "página indisponível");
        pagina_liberar(&p);
        return NULL;
    }
    char *texto = lia_web_html_para_texto(p.html, p.tamanho);
    char *titulo = lia_web_html_titulo(p.html);
    char *data = lia_web_html_data(p.html);
    JsonBuilder *b = json_builder_new(); json_builder_begin_object(b);
    add_str(b, "url", p.url_final ? p.url_final : url);
    if (titulo) add_str(b, "titulo", titulo);
    if (data) add_str(b, "data", data);
    add_str(b, "conteudo", texto);
    json_builder_set_member_name(b, "bytes_recebidos"); json_builder_add_int_value(b, (gint64)p.tamanho);
    json_builder_end_object(b);
    char *saida = json_para_texto(b);
    g_object_unref(b); g_free(texto); g_free(titulo); g_free(data);
    pagina_liberar(&p);
    return saida;
}

/* ------------------------------------------------------- cotações --- */

static char *pesquisar_cotacao(const char *consulta, char **erro_saida)
{
    char *t = normalizar_busca(consulta);
    const char *base = NULL;
    const char *destinos[4] = { NULL, NULL, NULL, NULL };
    guint n_destinos = 0;
    gboolean quer_dolar = strstr(t, " dolar") || strstr(t, " usd ");
    gboolean quer_real = strstr(t, " real ") || strstr(t, " reais ") || strstr(t, " brl ");
    gboolean quer_zimbabue = strstr(t, " zimb") || strstr(t, " zwl ");
    gboolean quer_euro = strstr(t, " euro") || strstr(t, " eur ");
    gboolean cotacao = strstr(t, " cotac") || strstr(t, " preco") || strstr(t, " quanto") || strstr(t, " vale") ||
                       strstr(t, " cambio") || strstr(t, " valor") || strstr(t, " em reais") || strstr(t, " em dolar");
    if (strstr(t, " libra") || strstr(t, " gbp ")) {
        base = "GBP";
        if (quer_dolar) destinos[n_destinos++] = "USD";
        if (quer_real) destinos[n_destinos++] = "BRL";
        if (quer_zimbabue) destinos[n_destinos++] = "ZWL";
        if (quer_euro) destinos[n_destinos++] = "EUR";
    } else if (quer_euro && (quer_real || quer_dolar)) {
        base = "EUR";
        if (quer_real) destinos[n_destinos++] = "BRL";
        if (quer_dolar) destinos[n_destinos++] = "USD";
    } else if (quer_dolar && quer_real) {
        base = "USD";
        destinos[n_destinos++] = "BRL";
    }
    g_free(t);
    if (!base || n_destinos == 0 || !cotacao) return NULL;

    gboolean usa_er_api = FALSE;
    for (guint i = 0; i < n_destinos; i++)
        if (!strcmp(destinos[i], "ZWL")) usa_er_api = TRUE;
    char *url = NULL;
    const char *fonte = NULL, *campo_data = NULL;
    if (usa_er_api) {
        url = g_strdup_printf("https://open.er-api.com/v6/latest/%s", base);
        fonte = "ExchangeRate-API Open Endpoint";
        campo_data = "time_last_update_utc";
    } else {
        char *simbolos = g_strjoinv(",", (char **)destinos);
        url = g_strdup_printf("https://api.frankfurter.dev/v1/latest?base=%s&symbols=%s", base, simbolos);
        g_free(simbolos);
        fonte = "Frankfurter / European Central Bank";
        campo_data = "date";
    }
    guint status = 0;
    char *validada = url_publica(url, erro_saida);
    GBytes *bytes = validada ? web_get(validada, WEB_TIMEOUT_BUSCA_SEG, &status, NULL, erro_saida) : NULL;
    g_free(validada);
    if (!bytes || status < 200 || status >= 300) {
        if (bytes) g_bytes_unref(bytes);
        if (erro_saida && !*erro_saida) *erro_saida = g_strdup_printf("API de câmbio respondeu HTTP %u", status);
        g_free(url);
        return NULL;
    }
    gsize n = 0; const char *raw = g_bytes_get_data(bytes, &n);
    JsonParser *p = json_parser_new();
    gboolean ok = json_parser_load_from_data(p, raw, (gssize)n, NULL) && json_parser_get_root(p) &&
                  JSON_NODE_TYPE(json_parser_get_root(p)) == JSON_NODE_OBJECT;
    JsonObject *o = ok ? json_node_get_object(json_parser_get_root(p)) : NULL;
    JsonObject *rates = o && json_object_has_member(o, "rates") &&
                        JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, "rates")) ? json_object_get_object_member(o, "rates") : NULL;
    gboolean completo = rates != NULL;
    for (guint i = 0; completo && i < n_destinos; i++) {
        JsonNode *valor = json_object_has_member(rates, destinos[i]) ? json_object_get_member(rates, destinos[i]) : NULL;
        completo = valor && JSON_NODE_TYPE(valor) == JSON_NODE_VALUE &&
                   (json_node_get_value_type(valor) == G_TYPE_DOUBLE || json_node_get_value_type(valor) == G_TYPE_INT64);
    }
    if (!completo) {
        if (erro_saida) *erro_saida = g_strdup("resposta de câmbio sem cotação estruturada para o par pedido");
        g_object_unref(p); g_bytes_unref(bytes); g_free(url); return NULL;
    }
    static const struct { const char *cod, *nome; } MOEDAS[] = {
        { "USD", "dólar americano" }, { "BRL", "real brasileiro" }, { "GBP", "libra esterlina" },
        { "EUR", "euro" }, { "ZWL", "dólar zimbabuano" },
    };
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    add_str(b, "consulta", consulta);
    add_str(b, "fonte", fonte);
    json_builder_set_member_name(b, "cotacoes"); json_builder_begin_array(b);
    for (guint i = 0; i < n_destinos; i++) {
        const char *nb = base, *nd = destinos[i];
        for (gsize k = 0; k < G_N_ELEMENTS(MOEDAS); k++) {
            if (!strcmp(MOEDAS[k].cod, base)) nb = MOEDAS[k].nome;
            if (!strcmp(MOEDAS[k].cod, destinos[i])) nd = MOEDAS[k].nome;
        }
        char *par = g_strdup_printf("%s para %s", nb, nd);
        JsonNode *valor = json_object_get_member(rates, destinos[i]);
        json_builder_begin_object(b);
        add_str(b, "par", par);
        add_str(b, "base", base);
        add_str(b, "destino", destinos[i]);
        json_builder_set_member_name(b, "valor");
        json_builder_add_double_value(b, json_node_get_value_type(valor) == G_TYPE_DOUBLE ? json_node_get_double(valor)
                                                                                          : (double)json_node_get_int(valor));
        add_str(b, "data", json_object_has_member(o, campo_data) && JSON_NODE_HOLDS_VALUE(json_object_get_member(o, campo_data)) ?
                           json_object_get_string_member(o, campo_data) : "data desconhecida");
        json_builder_end_object(b);
        g_free(par);
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "resultados"); json_builder_begin_array(b);
    json_builder_begin_object(b);
    add_str(b, "titulo", fonte);
    add_str(b, "url", url);
    add_str(b, "resumo", "cotação estruturada com data de referência");
    json_builder_end_object(b); json_builder_end_array(b);
    json_builder_end_object(b);
    char *saida = json_para_texto(b);
    g_object_unref(b); g_object_unref(p); g_bytes_unref(bytes); g_free(url);
    return saida;
}

/* ------------------------------------------------------ web.pesquisar --- */

static char *extrair_url(const char *href)
{
    if (!href) return NULL;
    char *url = decodificar_entidades(href);
    if (g_str_has_prefix(url, "//") || strstr(url, "duckduckgo.com/l/")) {
        char *p = strstr(url, "uddg=");
        if (p) {
            p += 5;
            char *fim = strchr(p, '&');
            char *cod = fim ? g_strndup(p, (gsize)(fim - p)) : g_strdup(p);
            char *dec = g_uri_unescape_string(cod, NULL);
            g_free(cod); g_free(url); url = dec;
        } else if (g_str_has_prefix(url, "//")) {
            char *absoluta = g_strdup_printf("https:%s", url);
            g_free(url); url = absoluta;
        }
    }
    if (!url || !g_str_has_prefix(url, "https://")) { g_free(url); return NULL; }
    return url;
}

typedef struct {
    char *titulo, *url, *resumo;
    int relevancia;
    /* aprofundamento */
    gboolean abrir, aberta;
    char *conteudo, *data, *erro_abertura;
    const char *consulta;
} ResultadoBusca;

static void resultado_busca_liberar(gpointer dados)
{
    ResultadoBusca *r = dados;
    g_free(r->titulo); g_free(r->url); g_free(r->resumo);
    g_free(r->conteudo); g_free(r->data); g_free(r->erro_abertura);
    g_free(r);
}

static gint comparar_relevancia(gconstpointer a, gconstpointer b)
{
    const ResultadoBusca *ra = *(ResultadoBusca * const *)a;
    const ResultadoBusca *rb = *(ResultadoBusca * const *)b;
    return rb->relevancia - ra->relevancia;
}

static GPtrArray *extrair_resultados(const char *html, const char *consulta)
{
    GPtrArray *resultados = g_ptr_array_new_with_free_func(resultado_busca_liberar);
    GRegex *links = g_regex_new("(?is)<a[^>]*class=[\\\"'][^\\\"']*result__a[^\\\"']*[\\\"'][^>]*href=[\\\"']([^\\\"']+)[\\\"'][^>]*>(.*?)</a>.*?<a[^>]*class=[\\\"'][^\\\"']*result__snippet[^\\\"']*[\\\"'][^>]*>(.*?)</a>", 0, 0, NULL);
    if (!links) return resultados;
    char **termos = lia_web_termos(consulta);
    GMatchInfo *mi = NULL;
    g_regex_match(links, html, 0, &mi);
    while (resultados->len < 30 && g_match_info_matches(mi)) {
        char *href = g_match_info_fetch(mi, 1);
        char *titulo_html = g_match_info_fetch(mi, 2);
        char *resumo_html = g_match_info_fetch(mi, 3);
        char *url = extrair_url(href);
        char *titulo = html_limpar(titulo_html);
        char *resumo = html_limpar(resumo_html);
        gboolean anuncio = url && (strstr(url, "duckduckgo.com/y.js") || strstr(url, "ad_domain=") ||
                                    strstr(url, "ad_provider=") || strstr(url, "ad_type="));
        gboolean repetido = FALSE;
        for (guint i = 0; url && i < resultados->len; i++)
            if (!strcmp(((ResultadoBusca *)g_ptr_array_index(resultados, i))->url, url)) repetido = TRUE;
        if (url && *titulo && !anuncio && !repetido) {
            ResultadoBusca *r = g_new0(ResultadoBusca, 1);
            r->titulo = titulo; titulo = NULL;
            r->url = url; url = NULL;
            r->resumo = resumo; resumo = NULL;
            r->relevancia = relevancia_resultado(termos, r->titulo, r->resumo);
            /* enciclopédias e documentação oficial tendem a ter o texto mais
             * completo e estável para perguntas de conhecimento */
            if (strstr(r->url, "wikipedia.org/")) r->relevancia += 4;
            g_ptr_array_add(resultados, r);
        }
        g_free(href); g_free(titulo_html); g_free(resumo_html); g_free(url); g_free(titulo); g_free(resumo);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(links);
    g_strfreev(termos);
    g_ptr_array_sort(resultados, comparar_relevancia);
    return resultados;
}

static gpointer abrir_resultado(gpointer dados);

/* abre em paralelo; se uma thread não puder ser criada (limite de memória
 * do daemon), abre aquela fonte na própria thread — nunca aborta */
static void abrir_em_thread(GPtrArray *threads, ResultadoBusca *r)
{
    GThread *t = g_thread_try_new("lia-web-abrir", abrir_resultado, r, NULL);
    if (t) g_ptr_array_add(threads, t);
    else abrir_resultado(r);
}

static gpointer abrir_resultado(gpointer dados)
{
    ResultadoBusca *r = dados;
    Pagina p = { .url = g_strdup(r->url) };
    pagina_baixar(&p, WEB_TIMEOUT_PAGINA_SEG);
    if (p.html) {
        char *texto = lia_web_html_para_texto(p.html, p.tamanho);
        r->conteudo = lia_web_trecho_relevante(texto, r->consulta, WEB_MAX_TRECHO);
        r->data = lia_web_html_data(p.html);
        r->aberta = r->conteudo && g_utf8_strlen(r->conteudo, -1) >= 80;
        if (!r->aberta) {
            r->erro_abertura = g_strdup("a página não tinha texto legível (talvez dependa de JavaScript)");
            g_clear_pointer(&r->conteudo, g_free);
        }
        g_free(texto);
    } else {
        r->erro_abertura = g_strdup(p.erro ? p.erro : "página indisponível");
    }
    pagina_liberar(&p);
    return NULL;
}

/* host sem "www." — para não abrir duas páginas do mesmo site */
static char *dominio(const char *url)
{
    GUri *u = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    char *h = g_strdup(u && g_uri_get_host(u) ? g_uri_get_host(u) : "");
    if (u) g_uri_unref(u);
    if (g_str_has_prefix(h, "www.")) memmove(h, h + 4, strlen(h + 4) + 1);
    return h;
}

char *lia_web_pesquisar(const char *consulta, int limite, int abrir_fontes, char **erro_saida)
{
    if (erro_saida) *erro_saida = NULL;
    if (!consulta || !*consulta || strlen(consulta) > WEB_MAX_CONSULTA) {
        if (erro_saida) *erro_saida = g_strdup("consulta vazia ou acima de 512 bytes");
        return NULL;
    }
    if (limite < 1 || limite > 10) limite = 5;
    if (abrir_fontes < 0) abrir_fontes = 0;
    if (abrir_fontes > WEB_MAX_ABRIR) abrir_fontes = WEB_MAX_ABRIR;

    char *erro_cotacao = NULL;
    char *cotacao = pesquisar_cotacao(consulta, &erro_cotacao);
    if (cotacao) return cotacao;
    g_free(erro_cotacao); /* a API de câmbio falhou: a busca geral ainda pode responder */

    char *codificada = g_uri_escape_string(consulta, NULL, TRUE);
    char *url = g_strdup_printf(WEB_URL_BASE "%s&kl=br-pt", codificada);
    g_free(codificada);
    guint status = 0;
    GBytes *bytes = web_get(url, WEB_TIMEOUT_BUSCA_SEG, &status, NULL, erro_saida);
    g_free(url);
    if (!bytes || status < 200 || status >= 300) {
        if (erro_saida && !*erro_saida) *erro_saida = g_strdup_printf("pesquisa web falhou (HTTP %u)", status);
        if (bytes) g_bytes_unref(bytes);
        return NULL;
    }
    gsize tamanho = 0;
    const char *corpo = g_bytes_get_data(bytes, &tamanho);
    if (tamanho > WEB_MAX_CORPO) {
        if (erro_saida) *erro_saida = g_strdup("resposta web excedeu 2 MiB");
        g_bytes_unref(bytes);
        return NULL;
    }
    char *html = g_strndup(corpo, tamanho);
    g_bytes_unref(bytes);
    GPtrArray *resultados = extrair_resultados(html, consulta);
    g_free(html);
    guint n = MIN(resultados->len, (guint)limite);

    /* aprofundamento: abre em paralelo as melhores fontes (domínios
     * distintos), cada uma com seu próprio tempo-limite */
    GPtrArray *threads = g_ptr_array_new();
    GPtrArray *dominios = g_ptr_array_new_with_free_func(g_free);
    int primeira = 0;
    for (guint i = 0; i < n && primeira < abrir_fontes; i++) {
        ResultadoBusca *r = g_ptr_array_index(resultados, i);
        char *d = dominio(r->url);
        gboolean repetido = FALSE;
        for (guint k = 0; k < dominios->len; k++) if (!strcmp(g_ptr_array_index(dominios, k), d)) repetido = TRUE;
        if (repetido) { g_free(d); continue; }
        g_ptr_array_add(dominios, d);
        r->abrir = TRUE;
        r->consulta = consulta;
        primeira++;
        abrir_em_thread(threads, r);
    }
    for (guint i = 0; i < threads->len; i++) g_thread_join(g_ptr_array_index(threads, i));
    /* segunda rodada: as que falharam (404, sem texto legível) são
     * substituídas pelas próximas da lista, até completar abrir_fontes */
    int ok = 0;
    for (guint i = 0; i < n; i++) if (((ResultadoBusca *)g_ptr_array_index(resultados, i))->aberta) ok++;
    g_ptr_array_set_size(threads, 0);
    int segunda = 0;
    for (guint i = 0; i < n && ok + segunda < abrir_fontes; i++) {
        ResultadoBusca *r = g_ptr_array_index(resultados, i);
        if (r->abrir) continue;
        char *d = dominio(r->url);
        gboolean repetido = FALSE;
        for (guint k = 0; k < dominios->len; k++) if (!strcmp(g_ptr_array_index(dominios, k), d)) repetido = TRUE;
        if (repetido) { g_free(d); continue; }
        g_ptr_array_add(dominios, d);
        r->abrir = TRUE;
        r->consulta = consulta;
        segunda++;
        abrir_em_thread(threads, r);
    }
    for (guint i = 0; i < threads->len; i++) g_thread_join(g_ptr_array_index(threads, i));
    g_ptr_array_free(threads, TRUE);
    g_ptr_array_unref(dominios);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    add_str(b, "consulta", consulta);
    add_str(b, "fonte", "DuckDuckGo HTML");
    int abertas = 0;
    json_builder_set_member_name(b, "resultados"); json_builder_begin_array(b);
    for (guint i = 0; i < n; i++) {
        ResultadoBusca *r = g_ptr_array_index(resultados, i);
        json_builder_begin_object(b);
        add_str(b, "titulo", r->titulo);
        add_str(b, "url", r->url);
        add_str(b, "resumo", r->resumo);
        json_builder_set_member_name(b, "aberta"); json_builder_add_boolean_value(b, r->aberta);
        if (r->aberta) { add_str(b, "conteudo", r->conteudo); abertas++; }
        if (r->data) add_str(b, "data", r->data);
        if (r->abrir && !r->aberta) add_str(b, "erro_abertura", r->erro_abertura ? r->erro_abertura : "não foi possível abrir");
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "fontes_abertas"); json_builder_add_int_value(b, abertas);
    json_builder_end_object(b);
    char *saida = json_para_texto(b);
    g_object_unref(b);
    g_ptr_array_unref(resultados);
    return saida;
}
