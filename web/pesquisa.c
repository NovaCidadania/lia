#include "pesquisa.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define WEB_URL_BASE "https://html.duckduckgo.com/html/?q="
#define WEB_MAX_CORPO (2 * 1024 * 1024)
#define WEB_MAX_CONSULTA 512
#define WEB_TIMEOUT_SEG 20

static char *html_limpar(const char *s)
{
    if (!s) return g_strdup("");
    GError *erro = NULL;
    GRegex *tags = g_regex_new("<[^>]*>", G_REGEX_DOTALL, 0, NULL);
    char *sem_tags = g_regex_replace(tags, s, -1, 0, "", 0, &erro);
    g_regex_unref(tags);
    if (erro) { g_error_free(erro); return g_strdup(s); }
    GString *dec = g_string_new(sem_tags ? sem_tags : "");
    g_free(sem_tags);
    g_string_replace(dec, "&amp;", "&", 0);
    g_string_replace(dec, "&quot;", "\"", 0);
    g_string_replace(dec, "&#39;", "'", 0);
    g_string_replace(dec, "&lt;", "<", 0);
    g_string_replace(dec, "&gt;", ">", 0);
    char *texto = g_string_free(dec, FALSE);
    g_strstrip(texto);
    return texto;
}

static char *extrair_url(const char *href)
{
    if (!href) return NULL;
    char *url = g_strdup(href);
    if (g_str_has_prefix(url, "//")) {
        char *p = strstr(url, "uddg=");
        if (p) {
            p += 5;
            char *fim = strchr(p, '&');
            char *cod = fim ? g_strndup(p, (gsize)(fim - p)) : g_strdup(p);
            char *dec = g_uri_unescape_string(cod, NULL);
            g_free(cod); g_free(url); url = dec;
        } else {
            char *absoluta = g_strdup_printf("https:%s", url);
            g_free(url); url = absoluta;
        }
    }
    if (!url || !g_str_has_prefix(url, "https://")) { g_free(url); return NULL; }
    return url;
}

static void adicionar_resultados(JsonBuilder *b, const char *html, int limite)
{
    GError *erro = NULL;
    GRegex *links = g_regex_new("(?is)<a[^>]*class=[\\\"']result__a[\\\"'][^>]*href=[\\\"']([^\\\"']+)[\\\"'][^>]*>(.*?)</a>", 0, 0, &erro);
    if (erro) { g_error_free(erro); return; }
    GMatchInfo *mi = NULL;
    g_regex_match(links, html, 0, &mi);
    int n = 0;
    while (n < limite && g_match_info_matches(mi)) {
        char *href = g_match_info_fetch(mi, 1);
        char *titulo_html = g_match_info_fetch(mi, 2);
        char *url = extrair_url(href);
        char *titulo = html_limpar(titulo_html);
        if (url && *titulo) {
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "titulo"); json_builder_add_string_value(b, titulo);
            json_builder_set_member_name(b, "url"); json_builder_add_string_value(b, url);
            json_builder_set_member_name(b, "resumo"); json_builder_add_string_value(b, "resultado encontrado na pesquisa web");
            json_builder_end_object(b);
            n++;
        }
        g_free(href); g_free(titulo_html); g_free(url); g_free(titulo);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi); g_regex_unref(links);
}

char *lia_web_pesquisar(const char *consulta, int limite, char **erro_saida)
{
    if (erro_saida) *erro_saida = NULL;
    if (!consulta || !*consulta || strlen(consulta) > WEB_MAX_CONSULTA) {
        if (erro_saida) *erro_saida = g_strdup("consulta vazia ou acima de 512 bytes");
        return NULL;
    }
    if (limite < 1 || limite > 10) limite = 5;

    char *codificada = g_uri_escape_string(consulta, NULL, TRUE);
    char *url = g_strdup_printf(WEB_URL_BASE "%s", codificada);
    g_free(codificada);

    SoupSession *sessao = soup_session_new();
    g_object_set(sessao, "timeout", WEB_TIMEOUT_SEG, "max-conns", 1, NULL);
    SoupMessage *msg = soup_message_new("GET", url);
    g_free(url);
    if (!msg) {
        g_object_unref(sessao);
        if (erro_saida) *erro_saida = g_strdup("não foi possível criar a requisição HTTPS");
        return NULL;
    }
    soup_message_headers_append(soup_message_get_request_headers(msg), "User-Agent", "Lia/1.0 (pesquisa local)");
    GError *erro = NULL;
    GBytes *bytes = soup_session_send_and_read(sessao, msg, NULL, &erro);
    guint status = soup_message_get_status(msg);
    if (!bytes || status < 200 || status >= 300) {
        if (erro_saida) *erro_saida = g_strdup_printf("pesquisa web falhou (HTTP %u): %s", status, erro ? erro->message : "sem resposta");
        if (erro) g_error_free(erro);
        if (bytes) g_bytes_unref(bytes);
        g_object_unref(msg); g_object_unref(sessao);
        return NULL;
    }
    gsize tamanho = 0;
    const char *corpo = g_bytes_get_data(bytes, &tamanho);
    if (tamanho > WEB_MAX_CORPO) {
        if (erro_saida) *erro_saida = g_strdup("resposta web excedeu 2 MiB");
        g_bytes_unref(bytes); g_object_unref(msg); g_object_unref(sessao);
        return NULL;
    }

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "consulta"); json_builder_add_string_value(b, consulta);
    json_builder_set_member_name(b, "fonte"); json_builder_add_string_value(b, "DuckDuckGo HTML");
    json_builder_set_member_name(b, "resultados"); json_builder_begin_array(b);
    char *html = g_strndup(corpo, tamanho);
    adicionar_resultados(b, html, limite);
    g_free(html);
    json_builder_end_array(b);
    json_builder_end_object(b);
    JsonNode *raiz = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, raiz);
    char *saida = json_generator_to_data(gen, NULL);
    json_node_unref(raiz); g_object_unref(gen); g_object_unref(b);
    g_bytes_unref(bytes); g_object_unref(msg); g_object_unref(sessao);
    return saida;
}
