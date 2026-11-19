#include "pesquisa.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define WEB_URL_BASE "https://html.duckduckgo.com/html/?q="
#define WEB_MAX_CORPO (2 * 1024 * 1024)
#define WEB_MAX_CONSULTA 512
#define WEB_TIMEOUT_SEG 20

static char *html_limpar(const char *s);

static GBytes *web_get(const char *url, guint *status, char **erro_saida)
{
    SoupSession *sessao = soup_session_new();
    g_object_set(sessao, "timeout", WEB_TIMEOUT_SEG, "max-redirects", 0, NULL);
    SoupMessage *msg = soup_message_new("GET", url);
    if (!msg) { g_object_unref(sessao); if (erro_saida) *erro_saida = g_strdup("requisição HTTPS inválida"); return NULL; }
    soup_message_headers_append(soup_message_get_request_headers(msg), "User-Agent", "Lia/1.0 (pesquisa local)");
    GError *erro = NULL;
    GBytes *bytes = soup_session_send_and_read(sessao, msg, NULL, &erro);
    if (status) *status = soup_message_get_status(msg);
    if (!bytes) {
        if (erro_saida) *erro_saida = g_strdup_printf("requisição HTTPS falhou: %s", erro ? erro->message : "sem resposta");
        if (erro) g_error_free(erro);
    }
    g_object_unref(msg); g_object_unref(sessao);
    return bytes;
}

static gboolean host_publico(const char *host, char **erro_saida)
{
    if (!host || !*host) { if (erro_saida) *erro_saida = g_strdup("URL sem host"); return FALSE; }
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
            g_inet_address_get_is_site_local(a) || g_inet_address_get_is_multicast(a)) { ok = FALSE; break; }
    }
    g_resolver_free_addresses(enderecos);
    if (!ok && erro_saida) *erro_saida = g_strdup("destino resolve para rede local, privada ou multicast — recusado");
    return ok;
}

static char *url_publica(const char *url, char **erro_saida)
{
    GError *erro = NULL;
    GUri *u = g_uri_parse(url, G_URI_FLAGS_NONE, &erro);
    if (!u) { if (erro_saida) *erro_saida = g_strdup_printf("URL inválida: %s", erro ? erro->message : "erro"); if (erro) g_error_free(erro); return NULL; }
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

static char *pagina_para_texto(const char *html, gsize tamanho)
{
    char *bruto = g_strndup(html, tamanho);
    GError *erro = NULL;
    GRegex *ocultos = g_regex_new("(?is)<(script|style|noscript)[^>]*>.*?</\\1>", 0, 0, &erro);
    char *sem_ocultos = erro ? g_strdup(bruto) : g_regex_replace(ocultos, bruto, -1, 0, "", 0, NULL);
    if (!erro) g_regex_unref(ocultos); else g_error_free(erro);
    g_free(bruto);
    char *texto = html_limpar(sem_ocultos);
    g_free(sem_ocultos);
    if (strlen(texto) > 120000) texto[120000] = '\0';
    return texto;
}

char *lia_web_abrir(const char *url, char **erro_saida)
{
    if (erro_saida) *erro_saida = NULL;
    char *segura = url_publica(url, erro_saida);
    if (!segura) return NULL;
    guint status = 0; GBytes *bytes = web_get(segura, &status, erro_saida);
    if (!bytes || status < 200 || status >= 300) {
        if (erro_saida && !*erro_saida) *erro_saida = g_strdup_printf("página respondeu HTTP %u; redirecionamentos exigem validação explícita", status);
        g_free(segura); if (bytes) g_bytes_unref(bytes); return NULL;
    }
    gsize tamanho = 0; const char *corpo = g_bytes_get_data(bytes, &tamanho);
    if (tamanho > WEB_MAX_CORPO) {
        if (erro_saida) *erro_saida = g_strdup("página excedeu 2 MiB");
        g_free(segura); g_bytes_unref(bytes); return NULL;
    }
    char *texto = pagina_para_texto(corpo, tamanho);
    JsonBuilder *b = json_builder_new(); json_builder_begin_object(b);
    json_builder_set_member_name(b, "url"); json_builder_add_string_value(b, segura);
    json_builder_set_member_name(b, "conteudo"); json_builder_add_string_value(b, texto);
    json_builder_set_member_name(b, "bytes_recebidos"); json_builder_add_int_value(b, (gint64)tamanho);
    json_builder_end_object(b);
    JsonNode *root = json_builder_get_root(b); JsonGenerator *g = json_generator_new(); json_generator_set_root(g, root);
    char *saida = json_generator_to_data(g, NULL);
    json_node_unref(root); g_object_unref(g); g_object_unref(b); g_free(texto); g_free(segura); g_bytes_unref(bytes);
    return saida;
}

static char *pesquisar_cotacao(const char *consulta, char **erro_saida)
{
    char *t = g_ascii_strdown(consulta, -1);
    const char *base = NULL;
    const char *destinos[3] = { NULL, NULL, NULL };
    guint n_destinos = 0;
    gboolean quer_dolar = strstr(t, "dolar") || strstr(t, "usd");
    gboolean quer_real = strstr(t, "real") || strstr(t, "reais") || strstr(t, "brl");
    gboolean quer_zimbabue = strstr(t, "zimb") || strstr(t, "zwl");
    if (strstr(t, "libra") || strstr(t, "gbp")) {
        base = "GBP";
        /* Uma única consulta permite responder, por exemplo, libra→dólar e libra→real. */
        if (quer_dolar) destinos[n_destinos++] = "USD";
        if (quer_real) destinos[n_destinos++] = "BRL";
        if (quer_zimbabue) destinos[n_destinos++] = "ZWL";
    } else if (quer_dolar && quer_real) {
        base = "USD";
        destinos[n_destinos++] = "BRL";
    }
    g_free(t);
    if (!base || n_destinos == 0) return NULL;

    gboolean usa_er_api = FALSE;
    for (guint i = 0; i < n_destinos; i++)
        if (!strcmp(destinos[i], "ZWL")) usa_er_api = TRUE;
    char *url = NULL;
    const char *fonte = NULL, *url_fonte = NULL, *campo_data = NULL;
    if (usa_er_api) {
        url = g_strdup_printf("https://open.er-api.com/v6/latest/%s", base);
        fonte = "ExchangeRate-API Open Endpoint";
        url_fonte = "https://open.er-api.com/v6/latest/GBP";
        campo_data = "time_last_update_utc";
    } else {
        char *simbolos = g_strjoinv(",", (char **)destinos);
        url = g_strdup_printf("https://api.frankfurter.dev/v1/latest?base=%s&symbols=%s", base, simbolos);
        g_free(simbolos);
        fonte = "Frankfurter / European Central Bank";
        url_fonte = "https://api.frankfurter.dev/v1/latest";
        campo_data = "date";
    }
    guint status = 0;
    GBytes *bytes = web_get(url, &status, erro_saida);
    g_free(url);
    if (!bytes || status < 200 || status >= 300) {
        if (bytes) g_bytes_unref(bytes);
        if (erro_saida && !*erro_saida) *erro_saida = g_strdup_printf("API de câmbio respondeu HTTP %u", status);
        return NULL;
    }
    gsize n = 0; const char *raw = g_bytes_get_data(bytes, &n);
    JsonParser *p = json_parser_new();
    gboolean ok = json_parser_load_from_data(p, raw, (gssize)n, NULL) && json_parser_get_root(p) &&
                  JSON_NODE_TYPE(json_parser_get_root(p)) == JSON_NODE_OBJECT;
    JsonObject *o = ok ? json_node_get_object(json_parser_get_root(p)) : NULL;
    JsonObject *rates = o && json_object_has_member(o, "rates") ? json_object_get_object_member(o, "rates") : NULL;
    if (!ok || !o || !rates) {
        if (erro_saida) *erro_saida = g_strdup("resposta de câmbio sem cotação estruturada");
        g_object_unref(p); g_bytes_unref(bytes); return NULL;
    }
    for (guint i = 0; i < n_destinos; i++) {
        JsonNode *valor = json_object_has_member(rates, destinos[i]) ? json_object_get_member(rates, destinos[i]) : NULL;
        if (!valor || JSON_NODE_TYPE(valor) != JSON_NODE_VALUE) {
            if (erro_saida) *erro_saida = g_strdup_printf("resposta de câmbio sem cotação para %s", destinos[i]);
            g_object_unref(p); g_bytes_unref(bytes); return NULL;
        }
    }
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "consulta"); json_builder_add_string_value(b, consulta);
    json_builder_set_member_name(b, "fonte"); json_builder_add_string_value(b, fonte);
    json_builder_set_member_name(b, "cotacoes"); json_builder_begin_array(b);
    for (guint i = 0; i < n_destinos; i++) {
        const char *destino = destinos[i];
        const char *nome = !strcmp(base, "GBP") && !strcmp(destino, "USD") ? "libra esterlina para dólar americano" :
                           !strcmp(base, "GBP") && !strcmp(destino, "BRL") ? "libra esterlina para real brasileiro" :
                           !strcmp(base, "GBP") && !strcmp(destino, "ZWL") ? "libra esterlina para dólar zimbabuano" :
                           "dólar americano para real brasileiro";
        JsonNode *valor = json_object_get_member(rates, destino);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "par"); json_builder_add_string_value(b, nome);
        json_builder_set_member_name(b, "base"); json_builder_add_string_value(b, base);
        json_builder_set_member_name(b, "destino"); json_builder_add_string_value(b, destino);
        json_builder_set_member_name(b, "valor");
        if (json_node_get_value_type(valor) == G_TYPE_DOUBLE) json_builder_add_double_value(b, json_node_get_double(valor));
        else json_builder_add_int_value(b, json_node_get_int(valor));
        json_builder_set_member_name(b, "data");
        json_builder_add_string_value(b, json_object_has_member(o, campo_data) ?
                                      json_object_get_string_member(o, campo_data) : "data desconhecida");
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "resultados"); json_builder_begin_array(b);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "titulo"); json_builder_add_string_value(b, fonte);
    json_builder_set_member_name(b, "url"); json_builder_add_string_value(b, url_fonte);
    json_builder_set_member_name(b, "resumo"); json_builder_add_string_value(b, "cotação estruturada com data de referência");
    json_builder_end_object(b); json_builder_end_array(b);
    json_builder_end_object(b);
    JsonNode *root = json_builder_get_root(b); JsonGenerator *g = json_generator_new();
    json_generator_set_root(g, root); char *saida = json_generator_to_data(g, NULL);
    json_node_unref(root); g_object_unref(g); g_object_unref(b); g_object_unref(p); g_bytes_unref(bytes);
    return saida;
}

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
    GRegex *links = g_regex_new("(?is)<a[^>]*class=[\\\"'][^\\\"']*result__a[^\\\"']*[\\\"'][^>]*href=[\\\"']([^\\\"']+)[\\\"'][^>]*>(.*?)</a>.*?<a[^>]*class=[\\\"'][^\\\"']*result__snippet[^\\\"']*[\\\"'][^>]*>(.*?)</a>", 0, 0, &erro);
    if (erro) { g_error_free(erro); return; }
    GMatchInfo *mi = NULL;
    g_regex_match(links, html, 0, &mi);
    int n = 0;
    while (n < limite && g_match_info_matches(mi)) {
        char *href = g_match_info_fetch(mi, 1);
        char *titulo_html = g_match_info_fetch(mi, 2);
        char *resumo_html = g_match_info_fetch(mi, 3);
        char *url = extrair_url(href);
        char *titulo = html_limpar(titulo_html);
        char *resumo = html_limpar(resumo_html);
        if (url && *titulo) {
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "titulo"); json_builder_add_string_value(b, titulo);
            json_builder_set_member_name(b, "url"); json_builder_add_string_value(b, url);
            json_builder_set_member_name(b, "resumo"); json_builder_add_string_value(b, resumo);
            json_builder_end_object(b);
            n++;
        }
        g_free(href); g_free(titulo_html); g_free(resumo_html); g_free(url); g_free(titulo); g_free(resumo);
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

    char *cotacao = pesquisar_cotacao(consulta, erro_saida);
    if (cotacao) return cotacao;
    if (erro_saida && *erro_saida) { g_clear_pointer(erro_saida, g_free); }

    char *codificada = g_uri_escape_string(consulta, NULL, TRUE);
    char *url = g_strdup_printf(WEB_URL_BASE "%s", codificada);
    g_free(codificada);

    guint status = 0;
    GBytes *bytes = web_get(url, &status, erro_saida);
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
    g_bytes_unref(bytes);
    return saida;
}
