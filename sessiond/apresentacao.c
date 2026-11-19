/* lia-sessiond — implementação de sessiond/apresentacao.h. Ver o cabeçalho. */
#include "apresentacao.h"
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------- leitura --- */

static gint64 inteiro(JsonObject *o, const char *chave, gint64 padrao)
{
    if (!o || !json_object_has_member(o, chave)) return padrao;
    JsonNode *n = json_object_get_member(o, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return padrao;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_INT64) return json_node_get_int(n);
    if (t == G_TYPE_DOUBLE) return (gint64)json_node_get_double(n);
    return padrao;
}

static gboolean tem_inteiro(JsonObject *o, const char *chave)
{
    if (!o || !json_object_has_member(o, chave)) return FALSE;
    JsonNode *n = json_object_get_member(o, chave);
    return JSON_NODE_TYPE(n) == JSON_NODE_VALUE &&
           (json_node_get_value_type(n) == G_TYPE_INT64 || json_node_get_value_type(n) == G_TYPE_DOUBLE);
}

static const char *texto(JsonObject *o, const char *chave, const char *padrao)
{
    if (!o || !json_object_has_member(o, chave)) return padrao;
    JsonNode *n = json_object_get_member(o, chave);
    if (JSON_NODE_TYPE(n) != JSON_NODE_VALUE || json_node_get_value_type(n) != G_TYPE_STRING) return padrao;
    return json_node_get_string(n);
}

static JsonObject *objeto(JsonObject *o, const char *chave)
{
    if (!o || !json_object_has_member(o, chave)) return NULL;
    JsonNode *n = json_object_get_member(o, chave);
    return JSON_NODE_TYPE(n) == JSON_NODE_OBJECT ? json_node_get_object(n) : NULL;
}

static JsonArray *lista(JsonObject *o, const char *chave)
{
    if (!o || !json_object_has_member(o, chave)) return NULL;
    JsonNode *n = json_object_get_member(o, chave);
    return JSON_NODE_TYPE(n) == JSON_NODE_ARRAY ? json_node_get_array(n) : NULL;
}

/* "12,34" a partir de centésimos (vírgula decimal, pt-BR) */
static void centesimos(GString *out, gint64 c)
{
    g_string_append_printf(out, "%" G_GINT64_FORMAT ",%02d", c / 100, (int)(c % 100));
}

/* nome de processo/dispositivo vem do sistema (não confiável como texto):
 * nunca deixa caractere de controle chegar ao terminal ou ao prompt. */
/* 'largura' > 0: célula de tabela — completa com espaços e, se o texto for
 * maior, corta com "…" (o nome inteiro continua no JSON e na frase final);
 * largura == 0: sem corte nem preenchimento. */
static void anexar_seguro(GString *out, const char *s, gsize largura)
{
    gsize n = 0;
    for (const char *p = s ? s : ""; *p; p = g_utf8_next_char(p), n++) {
        if (largura > 0 && n == largura - 1 && g_utf8_strlen(s, -1) > (glong)largura) {
            g_string_append(out, "…");
            n++;
            break;
        }
        gunichar ch = g_utf8_get_char_validated(p, -1);
        if (ch == (gunichar)-1 || ch == (gunichar)-2) { g_string_append_c(out, '?'); break; }
        if (ch < 0x20 || ch == 0x7f) g_string_append_c(out, '?');
        else g_string_append_unichar(out, ch);
    }
    for (; n < largura; n++)
        g_string_append_c(out, ' ');
}

static const char *estado_legivel(const char *e)
{
    if (!e || !*e) return "?";
    switch (*e) {
    case 'R': return "executando";
    case 'S': return "dormindo";
    case 'D': return "em E/S";
    case 'I': return "ocioso";
    case 'Z': return "zumbi";
    case 'T': case 't': return "parado";
    default: return e;
    }
}

/* ------------------------------------------------------ renderização --- */

static void render_top_cpu(GString *out, JsonObject *d)
{
    JsonArray *procs = lista(d, "processos");
    guint n = procs ? json_array_get_length(procs) : 0;
    g_string_append_printf(out, "Processos que mais usam CPU agora — %u de %" G_GINT64_FORMAT
                           " processos amostrados (janela de %" G_GINT64_FORMAT " ms)\n",
                           n, inteiro(d, "amostrados", 0), inteiro(d, "intervalo_ms", 0));
    g_string_append(out, "   #  PID      NOME              ESTADO      CPU %   TEMPO DE CPU (s)\n");
    for (guint i = 0; i < n; i++) {
        JsonNode *el = json_array_get_element(procs, i);
        if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) continue;
        JsonObject *p = json_node_get_object(el);
        g_string_append_printf(out, "  %2u  %-8" G_GINT64_FORMAT " ", i + 1, inteiro(p, "pid", 0));
        anexar_seguro(out, texto(p, "nome", "?"), 17);
        g_string_append_c(out, ' ');
        anexar_seguro(out, estado_legivel(texto(p, "estado", "?")), 11);
        g_string_append_c(out, ' ');
        GString *pct = g_string_new(NULL);
        centesimos(pct, inteiro(p, "cpu_pct_centesimos", 0));
        g_string_append_printf(out, "%-7s %" G_GINT64_FORMAT "\n", pct->str, inteiro(p, "tempo_cpu_centesimos_s", 0) / 100);
        g_string_free(pct, TRUE);
    }
    if (n == 0)
        g_string_append(out, "  (nenhum processo com amostra válida nas duas leituras)\n");
    g_string_append(out, "CPU % é a fração da capacidade total da máquina (todos os núcleos) usada na janela de amostragem.\n");
}

static void render_top_memoria(GString *out, JsonObject *d)
{
    JsonArray *procs = lista(d, "processos");
    guint n = procs ? json_array_get_length(procs) : 0;
    g_string_append_printf(out, "Processos que mais usam memória RAM agora — %u de %" G_GINT64_FORMAT
                           " processos com memória de usuário (RAM total: %" G_GINT64_FORMAT " MiB)\n",
                           n, inteiro(d, "considerados", 0), inteiro(d, "memoria_total_kb", 0) / 1024);
    g_string_append(out, "   #  PID      NOME              ESTADO      RSS (MiB)   MEM %\n");
    for (guint i = 0; i < n; i++) {
        JsonNode *el = json_array_get_element(procs, i);
        if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) continue;
        JsonObject *p = json_node_get_object(el);
        g_string_append_printf(out, "  %2u  %-8" G_GINT64_FORMAT " ", i + 1, inteiro(p, "pid", 0));
        anexar_seguro(out, texto(p, "nome", "?"), 17);
        g_string_append_c(out, ' ');
        anexar_seguro(out, estado_legivel(texto(p, "estado", "?")), 11);
        g_string_append_printf(out, " %-11" G_GINT64_FORMAT " ", (inteiro(p, "rss_kb", 0) + 512) / 1024);
        centesimos(out, inteiro(p, "mem_pct_centesimos", 0));
        g_string_append_c(out, '\n');
    }
    if (n == 0)
        g_string_append(out, "  (nenhum processo com VmRSS legível)\n");
    g_string_append_printf(out, "RSS é a memória residente (VmRSS de /proc/<pid>/status). %" G_GINT64_FORMAT
                           " entradas sem memória de usuário (threads de kernel) ficaram fora.\n",
                           inteiro(d, "sem_memoria_de_usuario", 0));
}

static void render_recursos(GString *out, JsonObject *d)
{
    JsonObject *cpu = objeto(d, "cpu"), *mem = objeto(d, "memoria_kb"), *swap = objeto(d, "swap_kb");
    if (cpu)
        g_string_append_printf(out, "CPU: %" G_GINT64_FORMAT " núcleos — %s\n", inteiro(cpu, "nucleos", 0), texto(cpu, "modelo", "?"));
    if (mem) {
        gint64 total = inteiro(mem, "total", 0), disp = inteiro(mem, "disponivel", 0);
        g_string_append_printf(out, "RAM: %" G_GINT64_FORMAT " MiB no total; %" G_GINT64_FORMAT " MiB livres; %"
                               G_GINT64_FORMAT " MiB disponíveis", total / 1024, inteiro(mem, "livre", 0) / 1024, disp / 1024);
        if (total > 0) {
            g_string_append(out, " (");
            centesimos(out, (disp * 10000 + total / 2) / total);
            g_string_append(out, " % disponível)");
        }
        g_string_append_c(out, '\n');
    }
    if (swap) {
        gint64 total = inteiro(swap, "total", 0), livre = inteiro(swap, "livre", 0);
        if (total == 0)
            g_string_append(out, "Swap: não configurada (0 MiB)\n");
        else
            g_string_append_printf(out, "Swap: %" G_GINT64_FORMAT " MiB no total; %" G_GINT64_FORMAT " MiB livres; %"
                                   G_GINT64_FORMAT " MiB em uso\n", total / 1024, livre / 1024, (total - livre) / 1024);
    }
    if (tem_inteiro(d, "uptime_segundos") || tem_inteiro(d, "uptime_s")) {
        gint64 s = tem_inteiro(d, "uptime_segundos") ? inteiro(d, "uptime_segundos", 0) : inteiro(d, "uptime_s", 0);
        g_string_append_printf(out, "Uptime: %" G_GINT64_FORMAT " dias, %" G_GINT64_FORMAT " horas e %" G_GINT64_FORMAT
                               " minutos (%" G_GINT64_FORMAT " segundos)\n", s / 86400, (s % 86400) / 3600, (s % 3600) / 60, s);
    }
    JsonObject *carga = objeto(d, "carga_media_centesimos");
    if (carga) {
        g_string_append(out, "Carga média: ");
        centesimos(out, inteiro(carga, "1min", 0));
        g_string_append(out, " (1 min), ");
        centesimos(out, inteiro(carga, "5min", 0));
        g_string_append(out, " (5 min), ");
        centesimos(out, inteiro(carga, "15min", 0));
        g_string_append(out, " (15 min)\n");
    } else {
        g_string_append(out, "Carga média: não disponível nesta coleta\n");
    }
}

static void render_diagnosticar(GString *out, JsonObject *d)
{
    JsonObject *k = objeto(d, "kernel"), *cpu = objeto(d, "cpu"), *mem = objeto(d, "memoria_kb"), *u = objeto(d, "usuario");
    g_string_append_printf(out, "Distribuição: %s\n", texto(d, "distro", "?"));
    if (k)
        g_string_append_printf(out, "Kernel: %s %s (%s)\n", texto(k, "sysname", "?"), texto(k, "release", "?"), texto(k, "machine", "?"));
    if (cpu)
        g_string_append_printf(out, "CPU: %s — %" G_GINT64_FORMAT " núcleos\n", texto(cpu, "modelo", "?"), inteiro(cpu, "nucleos", 0));
    if (mem)
        g_string_append_printf(out, "Memória: %" G_GINT64_FORMAT " MiB no total; %" G_GINT64_FORMAT " MiB disponíveis\n",
                               inteiro(mem, "total", 0) / 1024, inteiro(mem, "disponivel", 0) / 1024);
    if (u)
        g_string_append_printf(out, "Usuário do daemon: uid %" G_GINT64_FORMAT " (euid %" G_GINT64_FORMAT ")\n",
                               inteiro(u, "uid", -1), inteiro(u, "euid", -1));
}

static void render_dispositivos(GString *out, JsonArray *arr)
{
    static const struct { const char *classe, *titulo; } CLASSES[] = {
        { "rede", "Interfaces de rede" }, { "bloco", "Dispositivos de bloco" }, { "pci", "Dispositivos PCI" },
    };
    guint n = json_array_get_length(arr);
    for (gsize c = 0; c < G_N_ELEMENTS(CLASSES); c++) {
        guint qtd = 0;
        for (guint i = 0; i < n; i++) {
            JsonNode *el = json_array_get_element(arr, i);
            if (JSON_NODE_TYPE(el) == JSON_NODE_OBJECT &&
                g_strcmp0(texto(json_node_get_object(el), "classe", ""), CLASSES[c].classe) == 0)
                qtd++;
        }
        if (qtd == 0) continue;
        g_string_append_printf(out, "%s (%u):\n", CLASSES[c].titulo, qtd);
        for (guint i = 0; i < n; i++) {
            JsonNode *el = json_array_get_element(arr, i);
            if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) continue;
            JsonObject *o = json_node_get_object(el);
            if (g_strcmp0(texto(o, "classe", ""), CLASSES[c].classe) != 0) continue;
            g_string_append(out, "  ");
            anexar_seguro(out, texto(o, "nome", "?"), 0);
            const char *v = texto(o, "vendor", NULL), *dv = texto(o, "dispositivo", NULL);
            if (v || dv)
                g_string_append_printf(out, "  (fabricante %s, dispositivo %s)", v ? v : "?", dv ? dv : "?");
            g_string_append_c(out, '\n');
        }
    }
    if (n == 0)
        g_string_append(out, "Nenhum dispositivo encontrado nesta classe.\n");
}

static void render_web(GString *out, JsonObject *d)
{
    const char *fonte = texto(d, "fonte", "fonte não informada");
    JsonArray *arr = lista(d, "resultados");
    JsonArray *cotacoes = lista(d, "cotacoes");
    guint n_cotacoes = cotacoes ? json_array_get_length(cotacoes) : 0;
    guint n_resultados = arr ? json_array_get_length(arr) : 0;
    if (json_object_has_member(d, "conteudo")) {
        g_string_append(out, "Conteúdo da página:\n");
        anexar_seguro(out, texto(d, "conteudo", ""), 0);
        g_string_append_c(out, '\n');
    }
    if (n_cotacoes) g_string_append(out, "Resposta\n");
    for (guint i = 0; i < n_cotacoes; i++) {
        JsonObject *c = json_array_get_object_element(cotacoes, i);
        double taxa = json_object_get_double_member(c, "valor");
        char valor[64];
        g_ascii_formatd(valor, sizeof valor, "%.4f", taxa);
        char *decimal = strchr(valor, '.');
        if (decimal) *decimal = ',';
        const char *base = texto(c, "base", "?"), *destino = texto(c, "destino", "?");
        g_string_append_printf(out, "1 %s = %s %s\n", base, valor, destino);
        g_string_append_printf(out, "Data de referência: %s\n", texto(c, "data", "data desconhecida"));
        /* A atribuição fica junto do valor, sem os rótulos técnicos do JSON. */
        g_string_append(out, "Fonte: ");
        anexar_seguro(out, fonte, 0);
        g_string_append_c(out, '\n');
    }
    if (!n_cotacoes && n_resultados > 0) {
        JsonNode *el = json_array_get_element(arr, 0);
        JsonObject *o = JSON_NODE_TYPE(el) == JSON_NODE_OBJECT ? json_node_get_object(el) : NULL;
        const char *titulo = texto(o, "titulo", "Resultado da pesquisa");
        const char *resumo = texto(o, "resumo", "");
        g_string_append(out, "Resposta\n");
        if (*resumo && strcmp(resumo, "resultado encontrado na pesquisa web") != 0)
            anexar_seguro(out, resumo, 0);
        else
            anexar_seguro(out, titulo, 0);
        g_string_append_c(out, '\n');
        g_string_append(out, "Fonte: ");
        anexar_seguro(out, titulo, 0);
        g_string_append_c(out, '\n');
        const char *url = texto(o, "url", NULL);
        if (url && *url) {
            g_string_append(out, "Link: ");
            anexar_seguro(out, url, 0);
            g_string_append_c(out, '\n');
        }
    }
    if (!n_cotacoes && n_resultados == 0)
        g_string_append(out, "A busca não encontrou uma resposta estruturada.\n");
}

static void render_generico(GString *out, JsonNode *n)
{
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, n);
    char *s = json_generator_to_data(gen, NULL);
    g_string_append(out, s ? s : "(sem dados)");
    g_string_append_c(out, '\n');
    g_free(s);
    g_object_unref(gen);
}

char *apresentacao_renderizar(const char *ferramenta, const char *dados_json, gboolean *truncado)
{
    if (truncado) *truncado = FALSE;
    GString *out = g_string_new(NULL);
    JsonParser *p = json_parser_new();
    if (!dados_json || !json_parser_load_from_data(p, dados_json, -1, NULL) || !json_parser_get_root(p)) {
        g_string_append(out, "(a ferramenta não devolveu dados estruturados legíveis)\n");
    } else {
        JsonNode *raiz = json_parser_get_root(p);
        JsonNode *carga = raiz;
        if (JSON_NODE_TYPE(raiz) == JSON_NODE_OBJECT) {
            JsonObject *o = json_node_get_object(raiz);
            if (json_object_has_member(o, "saida"))
                carga = json_object_get_member(o, "saida");
            else if (json_object_has_member(o, "texto"))
                carga = json_object_get_member(o, "texto");
        }
        const char *f = ferramenta ? ferramenta : "";
        if (JSON_NODE_TYPE(carga) == JSON_NODE_OBJECT && strcmp(f, "sistema.processos_top_cpu") == 0)
            render_top_cpu(out, json_node_get_object(carga));
        else if (JSON_NODE_TYPE(carga) == JSON_NODE_OBJECT && strcmp(f, "sistema.processos_top_memoria") == 0)
            render_top_memoria(out, json_node_get_object(carga));
        else if (JSON_NODE_TYPE(carga) == JSON_NODE_OBJECT && strcmp(f, "sistema.recursos") == 0)
            render_recursos(out, json_node_get_object(carga));
        else if (JSON_NODE_TYPE(carga) == JSON_NODE_OBJECT && strcmp(f, "sistema.diagnosticar") == 0)
            render_diagnosticar(out, json_node_get_object(carga));
        else if (JSON_NODE_TYPE(carga) == JSON_NODE_ARRAY && strcmp(f, "sistema.dispositivos") == 0)
            render_dispositivos(out, json_node_get_array(carga));
        else if (JSON_NODE_TYPE(carga) == JSON_NODE_OBJECT &&
                 (strcmp(f, "web.pesquisar") == 0 || strcmp(f, "web.abrir") == 0))
            render_web(out, json_node_get_object(carga));
        else if (JSON_NODE_TYPE(carga) == JSON_NODE_VALUE && json_node_get_value_type(carga) == G_TYPE_STRING) {
            for (const char *q = json_node_get_string(carga); *q; q++) /* texto humano da ferramenta: tira controle, mantém \n e \t */
                g_string_append_c(out, ((unsigned char)*q < 0x20 && *q != '\n' && *q != '\t') || *q == 0x7f ? '?' : *q);
            if (out->len && out->str[out->len - 1] != '\n') g_string_append_c(out, '\n');
        } else
            render_generico(out, carga);
    }
    g_object_unref(p);

    if (out->len > LIA_APRESENTACAO_LIMITE_BYTES) {
        gsize corte = LIA_APRESENTACAO_LIMITE_BYTES;
        while (corte > 0 && out->str[corte - 1] != '\n') corte--; /* corta em fim de linha: nunca no meio de um número/UTF-8 */
        if (corte == 0) {
            corte = LIA_APRESENTACAO_LIMITE_BYTES;
            while (corte > 0 && ((unsigned char)out->str[corte] & 0xC0) == 0x80) corte--;
        }
        gsize original = out->len;
        g_string_truncate(out, corte);
        g_string_append_printf(out, "[SAÍDA TRUNCADA: mostrados %zu de %zu bytes — a lista acima está INCOMPLETA]\n", corte, original);
        if (truncado) *truncado = TRUE;
    }
    return g_string_free(out, FALSE);
}

/* ---------------------------------------------------- fundamentação --- */

typedef struct {
    double valor;
    int casas;      /* casas decimais citadas */
    char bruto[40]; /* como apareceu */
} Numero;

/* Extrai os números "soltos" de 's'. Um número colado a uma letra/_ antes
 * dele ("ext4", "python3", "x86_64") faz parte de um nome, não é um valor.
 * Com 'pular_lista', ignora numeração no começo da linha ("1.", "2)"). */
static GArray *extrair_numeros(const char *s, gboolean pular_lista)
{
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(Numero));
    gboolean inicio_linha = TRUE;
    for (const char *p = s ? s : ""; *p;) {
        if (*p == '\n') { inicio_linha = TRUE; p++; continue; }
        if (!g_ascii_isdigit(*p)) {
            if (!(*p == ' ' || *p == '\t' || *p == '-' || *p == '*')) inicio_linha = FALSE;
            p++;
            continue;
        }
        gboolean colado = p > s && (g_ascii_isalpha(p[-1]) || p[-1] == '_' || ((unsigned char)p[-1] & 0x80));
        const char *ini = p;
        while (g_ascii_isdigit(*p)) p++;
        /* separadores internos: 1.234,56 / 1,234.56 / 15,83 */
        const char *fim = p;
        while ((*fim == '.' || *fim == ',') && g_ascii_isdigit(fim[1])) {
            fim++;
            while (g_ascii_isdigit(*fim)) fim++;
        }
        gboolean era_inicio = inicio_linha;
        inicio_linha = FALSE;
        if (colado) { p = fim; continue; }
        if (pular_lista && era_inicio && fim == p && (*fim == '.' || *fim == ')') && (fim[1] == ' ' || fim[1] == '\t')) {
            p = fim + 1; /* "3. item" */
            continue;
        }
        Numero n = { 0 };
        gsize tam = (gsize)(fim - ini);
        if (tam >= sizeof n.bruto) { p = fim; continue; }
        memcpy(n.bruto, ini, tam);
        /* interpreta: o ÚLTIMO separador é decimal se houver só um e ele
         * não for seguido de exatamente 3 dígitos; senão tudo é milhar. */
        int nsep = 0; const char *ultimo = NULL;
        for (const char *q = ini; q < fim; q++) if (*q == '.' || *q == ',') { nsep++; ultimo = q; }
        gboolean decimal = nsep == 1 && (fim - ultimo - 1) != 3;
        if (nsep > 1) {
            /* 1.234,56: separadores diferentes -> o último é decimal */
            char primeiro_sep = 0;
            for (const char *q = ini; q < fim; q++) if (*q == '.' || *q == ',') { primeiro_sep = *q; break; }
            decimal = *ultimo != primeiro_sep;
        }
        char limpo[40]; gsize k = 0;
        for (const char *q = ini; q < fim; q++) {
            if (*q == '.' || *q == ',') {
                if (decimal && q == ultimo) limpo[k++] = '.';
            } else
                limpo[k++] = *q;
        }
        limpo[k] = 0;
        n.valor = g_ascii_strtod(limpo, NULL);
        n.casas = decimal ? (int)(fim - ultimo - 1) : 0;
        g_array_append_val(arr, n);
        /* um número com um único separador seguido de 3 dígitos é ambíguo
         * (15.915 = quinze mil ou quinze vírgula nove?): registra as duas
         * leituras, para a comparação aceitar qualquer uma que exista. */
        if (nsep == 1 && !decimal) {
            Numero alt = n;
            char l2[40]; gsize j = 0;
            for (const char *q = ini; q < fim; q++) l2[j++] = (*q == ',') ? '.' : *q;
            l2[j] = 0;
            alt.valor = g_ascii_strtod(l2, NULL);
            alt.casas = 3;
            g_array_append_val(arr, alt);
        }
        p = fim;
    }
    return arr;
}

gboolean apresentacao_resumo_fundamentado(const char *resumo, const char *const *evidencias, char **suspeito)
{
    if (suspeito) *suspeito = NULL;
    GArray *ev = g_array_new(FALSE, FALSE, sizeof(Numero));
    for (int i = 0; evidencias && evidencias[i]; i++) {
        GArray *parte = extrair_numeros(evidencias[i], FALSE);
        g_array_append_vals(ev, parte->data, parte->len);
        g_array_free(parte, TRUE);
    }
    GArray *citados = extrair_numeros(resumo, TRUE);
    gboolean ok = TRUE;
    for (guint i = 0; i < citados->len && ok; i++) {
        Numero *c = &g_array_index(citados, Numero, i);
        if (c->casas == 0 && c->valor < 10 && strlen(c->bruto) == 1)
            continue; /* dígito isolado: contagem/ordinal trivial */
        /* leituras alternativas do MESMO texto (15.915) valem juntas */
        gboolean achou = FALSE;
        guint j_fim = i;
        while (j_fim + 1 < citados->len && strcmp(g_array_index(citados, Numero, j_fim + 1).bruto, c->bruto) == 0 &&
               g_array_index(citados, Numero, j_fim + 1).casas == 3 && c->casas == 0)
            j_fim++;
        for (guint a = i; a <= j_fim && !achou; a++) {
            Numero *alt = &g_array_index(citados, Numero, a);
            double tol = pow(10.0, -alt->casas) * 1.0000001; /* arredondado ou truncado na última casa citada */
            for (guint k = 0; k < ev->len && !achou; k++) {
                Numero *e = &g_array_index(ev, Numero, k);
                if (e->casas < alt->casas) {
                    if (fabs(e->valor - alt->valor) < 1e-9) achou = TRUE; /* "15,80" para 15,8 */
                } else if (alt->casas == e->casas ? fabs(e->valor - alt->valor) < 1e-9 : fabs(e->valor - alt->valor) < tol)
                    achou = TRUE;
            }
        }
        if (!achou) {
            ok = FALSE;
            if (suspeito) *suspeito = g_strdup(c->bruto);
        }
        i = j_fim;
    }
    g_array_free(citados, TRUE);
    g_array_free(ev, TRUE);
    return ok;
}

/* ------------------------------------------------- ranking e frase --- */

/* devolve a carga útil ("saida") do resultado, com o parser que a mantém viva */
static JsonNode *carga_util(const char *dados_json, JsonParser **parser)
{
    *parser = json_parser_new();
    if (!dados_json || !json_parser_load_from_data(*parser, dados_json, -1, NULL) || !json_parser_get_root(*parser))
        return NULL;
    JsonNode *raiz = json_parser_get_root(*parser);
    if (JSON_NODE_TYPE(raiz) == JSON_NODE_OBJECT && json_object_has_member(json_node_get_object(raiz), "saida"))
        return json_object_get_member(json_node_get_object(raiz), "saida");
    return raiz;
}

static gboolean e_ranking(const char *ferramenta)
{
    return ferramenta && (strcmp(ferramenta, "sistema.processos_top_cpu") == 0 ||
                          strcmp(ferramenta, "sistema.processos_top_memoria") == 0);
}

/* 'nome' aparece em 'texto' como palavra inteira (não como pedaço de outra) */
static gboolean cita(const char *texto, const char *nome)
{
    gsize n = strlen(nome);
    if (n == 0) return FALSE;
    for (const char *p = strstr(texto, nome); p; p = strstr(p + 1, nome)) {
        gboolean antes = p == texto || !(g_ascii_isalnum(p[-1]) || p[-1] == '_' || p[-1] == '-' || ((unsigned char)p[-1] & 0x80));
        gboolean depois = !(g_ascii_isalnum(p[n]) || p[n] == '_' || ((unsigned char)p[n] & 0x80));
        if (antes && depois) return TRUE;
    }
    return FALSE;
}

gboolean apresentacao_resumo_respeita_ranking(const char *resumo, const char *ferramenta,
                                               const char *dados_json, char **motivo)
{
    if (motivo) *motivo = NULL;
    if (!e_ranking(ferramenta) || !resumo) return TRUE;
    JsonParser *p = NULL;
    JsonNode *carga = carga_util(dados_json, &p);
    gboolean ok = TRUE;
    JsonArray *procs = carga && JSON_NODE_TYPE(carga) == JSON_NODE_OBJECT ? lista(json_node_get_object(carga), "processos") : NULL;
    guint n = procs ? json_array_get_length(procs) : 0;
    /* k = quantos primeiros colocados seguidos são citados */
    guint k = 0;
    for (; k < n; k++) {
        JsonNode *el = json_array_get_element(procs, k);
        if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT || !cita(resumo, texto(json_node_get_object(el), "nome", "")))
            break;
    }
    /* qualquer nome citado que só aparece DEPOIS da posição k foi citado pulando alguém */
    for (guint i = k; i < n && ok; i++) {
        JsonNode *el = json_array_get_element(procs, i);
        if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) continue;
        const char *nome = texto(json_node_get_object(el), "nome", "");
        if (!cita(resumo, nome)) continue;
        gboolean ja_no_prefixo = FALSE;
        for (guint j = 0; j < k; j++) {
            JsonNode *ej = json_array_get_element(procs, j);
            if (JSON_NODE_TYPE(ej) == JSON_NODE_OBJECT && strcmp(texto(json_node_get_object(ej), "nome", ""), nome) == 0)
                ja_no_prefixo = TRUE;
        }
        if (!ja_no_prefixo) {
            ok = FALSE;
            if (motivo) {
                JsonNode *ek = json_array_get_element(procs, k);
                *motivo = g_strdup_printf("citava \"%s\" (%uº colocado) sem citar \"%s\" (%uº), que vem antes",
                    nome, i + 1, JSON_NODE_TYPE(ek) == JSON_NODE_OBJECT ? texto(json_node_get_object(ek), "nome", "?") : "?", k + 1);
            }
        }
    }
    g_object_unref(p);
    return ok;
}

static void frase_processo(GString *out, JsonObject *p, gboolean memoria, gboolean com_pid)
{
    anexar_seguro(out, texto(p, "nome", "?"), 0);
    if (com_pid) g_string_append_printf(out, " (PID %" G_GINT64_FORMAT ")", inteiro(p, "pid", 0));
    if (memoria) {
        g_string_append_printf(out, "%s%" G_GINT64_FORMAT " MiB", com_pid ? ", com " : " (", (inteiro(p, "rss_kb", 0) + 512) / 1024);
        if (com_pid) {
            g_string_append(out, " (");
            centesimos(out, inteiro(p, "mem_pct_centesimos", 0));
            g_string_append(out, " % da RAM)");
        } else
            g_string_append_c(out, ')');
    } else {
        g_string_append(out, com_pid ? ", com " : " (");
        centesimos(out, inteiro(p, "cpu_pct_centesimos", 0));
        g_string_append(out, com_pid ? " % de CPU" : " %)");
    }
}

char *apresentacao_frase(const char *ferramenta, const char *dados_json)
{
    GString *out = g_string_new(NULL);
    JsonParser *p = NULL;
    JsonNode *carga = carga_util(dados_json, &p);
    const char *f = ferramenta ? ferramenta : "";
    JsonObject *d = carga && JSON_NODE_TYPE(carga) == JSON_NODE_OBJECT ? json_node_get_object(carga) : NULL;

    if (d && e_ranking(f)) {
        gboolean memoria = strcmp(f, "sistema.processos_top_memoria") == 0;
        JsonArray *procs = lista(d, "processos");
        guint n = procs ? json_array_get_length(procs) : 0;
        if (n == 0) {
            g_string_append(out, "A coleta não devolveu nenhum processo.");
        } else {
            g_string_append_printf(out, "O processo que mais usa %s agora é ", memoria ? "memória RAM" : "CPU");
            frase_processo(out, json_array_get_object_element(procs, 0), memoria, TRUE);
            if (n >= 2) {
                g_string_append(out, "; em seguida ");
                g_string_append(out, n >= 3 ? "vêm " : "vem ");
                frase_processo(out, json_array_get_object_element(procs, 1), memoria, FALSE);
                if (n >= 3) {
                    g_string_append(out, " e ");
                    frase_processo(out, json_array_get_object_element(procs, 2), memoria, FALSE);
                }
            }
            g_string_append_printf(out, ". A tabela acima traz os %u listados.", n);
        }
    } else if (d && strcmp(f, "sistema.recursos") == 0) {
        JsonObject *mem = objeto(d, "memoria_kb"), *swap = objeto(d, "swap_kb"), *carga_m = objeto(d, "carga_media_centesimos");
        if (mem)
            g_string_append_printf(out, "Há %" G_GINT64_FORMAT " MiB de RAM disponíveis de %" G_GINT64_FORMAT " MiB no total",
                                   inteiro(mem, "disponivel", 0) / 1024, inteiro(mem, "total", 0) / 1024);
        if (swap && inteiro(swap, "total", 0) > 0)
            g_string_append_printf(out, "%s%" G_GINT64_FORMAT " MiB de swap livres de %" G_GINT64_FORMAT " MiB",
                                   out->len ? ", e " : "Há ", inteiro(swap, "livre", 0) / 1024, inteiro(swap, "total", 0) / 1024);
        else if (swap)
            g_string_append_printf(out, "%snão há swap configurada", out->len ? ", e " : "");
        if (out->len) g_string_append(out, ". ");
        if (carga_m) {
            g_string_append(out, "A carga média é ");
            centesimos(out, inteiro(carga_m, "1min", 0));
            g_string_append(out, " (1 min), ");
            centesimos(out, inteiro(carga_m, "5min", 0));
            g_string_append(out, " (5 min) e ");
            centesimos(out, inteiro(carga_m, "15min", 0));
            g_string_append(out, " (15 min)");
        }
        if (tem_inteiro(d, "uptime_segundos")) {
            gint64 s = inteiro(d, "uptime_segundos", 0);
            g_string_append_printf(out, "%s %" G_GINT64_FORMAT " dias, %" G_GINT64_FORMAT " horas e %" G_GINT64_FORMAT " minutos",
                                   carga_m ? ", e o sistema está ligado há" : "O sistema está ligado há",
                                   s / 86400, (s % 86400) / 3600, (s % 3600) / 60);
        }
        if (carga_m || tem_inteiro(d, "uptime_segundos")) g_string_append_c(out, '.');
    } else if (d && strcmp(f, "sistema.diagnosticar") == 0) {
        JsonObject *k = objeto(d, "kernel"), *cpu = objeto(d, "cpu");
        g_string_append_printf(out, "A máquina roda %s", texto(d, "distro", "?"));
        if (k) g_string_append_printf(out, " com kernel %s %s (%s)", texto(k, "sysname", "?"), texto(k, "release", "?"), texto(k, "machine", "?"));
        if (cpu) g_string_append_printf(out, "; a CPU é %s, com %" G_GINT64_FORMAT " núcleos", texto(cpu, "modelo", "?"), inteiro(cpu, "nucleos", 0));
        g_string_append_c(out, '.');
    } else if (carga && JSON_NODE_TYPE(carga) == JSON_NODE_ARRAY && strcmp(f, "sistema.dispositivos") == 0) {
        JsonArray *arr = json_node_get_array(carga);
        guint n = json_array_get_length(arr), rede = 0, bloco = 0, pci = 0;
        GString *nomes_rede = g_string_new(NULL), *nomes_bloco = g_string_new(NULL);
        for (guint i = 0; i < n; i++) {
            JsonNode *el = json_array_get_element(arr, i);
            if (JSON_NODE_TYPE(el) != JSON_NODE_OBJECT) continue;
            JsonObject *o = json_node_get_object(el);
            const char *c = texto(o, "classe", "");
            if (strcmp(c, "rede") == 0) { g_string_append_printf(nomes_rede, "%s", rede ? ", " : ""); anexar_seguro(nomes_rede, texto(o, "nome", "?"), 0); rede++; }
            else if (strcmp(c, "bloco") == 0) { g_string_append_printf(nomes_bloco, "%s", bloco ? ", " : ""); anexar_seguro(nomes_bloco, texto(o, "nome", "?"), 0); bloco++; }
            else if (strcmp(c, "pci") == 0) pci++;
        }
        if (n == 0) g_string_append(out, "Nenhum dispositivo foi encontrado nesta classe.");
        if (rede) g_string_append_printf(out, "Existem %u interfaces de rede: %s. ", rede, nomes_rede->str);
        if (bloco) g_string_append_printf(out, "Existem %u dispositivos de bloco: %s. ", bloco, nomes_bloco->str);
        if (pci) g_string_append_printf(out, "Existem %u dispositivos PCI, listados acima com fabricante e identificador.", pci);
        g_string_free(nomes_rede, TRUE);
        g_string_free(nomes_bloco, TRUE);
    }
    g_object_unref(p);
    if (out->len == 0)
        g_string_append_printf(out, "Os dados acima foram lidos agora desta máquina por '%s'; nenhum valor além deles foi acrescentado.", f);
    g_strchomp(out->str);
    out->len = strlen(out->str);
    return g_string_free(out, FALSE);
}

/* ------------------------------------------- modo e resposta extrativa --- */

RespostaModo apresentacao_modo_resposta(const char *ferramenta)
{
    const char *f = ferramenta ? ferramenta : "";
    if (e_ranking(f) || strcmp(f, "sistema.dispositivos") == 0)
        return RESPOSTA_DETERMINISTICA;
    if (strcmp(f, "web.pesquisar") == 0 || strcmp(f, "web.abrir") == 0)
        return RESPOSTA_DETERMINISTICA;
    if (strcmp(f, "sistema.recursos") == 0 || strcmp(f, "sistema.diagnosticar") == 0)
        return RESPOSTA_EXTRATIVA;
    return RESPOSTA_RESUMO_CONFERIDO;
}

static GPtrArray *linhas_nao_vazias(const char *apresentacao)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    char **partes = g_strsplit(apresentacao ? apresentacao : "", "\n", -1);
    for (int i = 0; partes[i]; i++) {
        char *l = g_strstrip(g_strdup(partes[i]));
        if (*l) g_ptr_array_add(arr, l);
        else g_free(l);
    }
    g_strfreev(partes);
    return arr;
}

char *apresentacao_numerar_linhas(const char *apresentacao, int *n_linhas)
{
    GPtrArray *arr = linhas_nao_vazias(apresentacao);
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < arr->len; i++)
        g_string_append_printf(out, "%u. %s\n", i + 1, (char *)g_ptr_array_index(arr, i));
    if (n_linhas) *n_linhas = (int)arr->len;
    g_ptr_array_free(arr, TRUE);
    return g_string_free(out, FALSE);
}

/* rótulo da linha renderizada (antes de ':') -> palavras do pedido que a pedem */
static const struct { const char *rotulo; const char *const palavras[8]; } CONCEITOS[] = {
    { "CPU", { "cpu", "processador", "núcleo", "nucleo", "modelo", NULL } },
    { "RAM", { " ram", "memória", "memoria", NULL } },
    { "Memória", { " ram", "memória", "memoria", NULL } },
    { "Swap", { "swap", NULL } },
    { "Uptime", { "uptime", "ligad", "tempo de atividade", NULL } },
    { "Carga média", { "carga", "load", NULL } },
    { "Distribuição", { "distribu", "distro", "sistema operacional", NULL } },
    { "Kernel", { "kernel", NULL } },
};

static gboolean linha_pedida(const char *linha, const char *pedido_minusculo)
{
    for (gsize c = 0; c < G_N_ELEMENTS(CONCEITOS); c++) {
        gsize n = strlen(CONCEITOS[c].rotulo);
        if (strncmp(linha, CONCEITOS[c].rotulo, n) != 0 || linha[n] != ':')
            continue;
        for (int k = 0; CONCEITOS[c].palavras[k]; k++)
            if (strstr(pedido_minusculo, CONCEITOS[c].palavras[k]))
                return TRUE;
    }
    return FALSE;
}

char *apresentacao_resposta_extrativa(const char *apresentacao, const char *pedido,
                                       const int *linhas_modelo, int n_modelo, int *do_modelo)
{
    GPtrArray *arr = linhas_nao_vazias(apresentacao);
    char *min = g_utf8_strdown(pedido ? pedido : "", -1);
    char *ped = g_strdup_printf(" %s ", min); /* espaço em volta: " ram" casa no começo do pedido também */
    g_free(min);
    gboolean *escolhida = g_new0(gboolean, arr->len + 1);
    int validas = 0, total = 0;
    for (int i = 0; i < n_modelo; i++) {
        int idx = linhas_modelo[i];
        if (idx >= 1 && idx <= (int)arr->len && !escolhida[idx - 1]) {
            escolhida[idx - 1] = TRUE;
            validas++;
        }
    }
    for (guint i = 0; i < arr->len; i++)
        if (linha_pedida(g_ptr_array_index(arr, i), ped))
            escolhida[i] = TRUE;
    for (guint i = 0; i < arr->len; i++)
        if (escolhida[i]) total++;
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < arr->len; i++) {
        if (total > 0 && !escolhida[i]) continue;
        const char *l = g_ptr_array_index(arr, i);
        g_string_append_printf(out, "%s%s", out->len ? " " : "", l);
        gsize n = strlen(l);
        if (n && l[n - 1] != '.') g_string_append_c(out, '.');
    }
    if (out->len == 0)
        g_string_append(out, "A ferramenta não devolveu nenhuma linha de dados.");
    if (do_modelo) *do_modelo = validas;
    g_free(escolhida);
    g_free(ped);
    g_ptr_array_free(arr, TRUE);
    return g_string_free(out, FALSE);
}

gboolean apresentacao_resumo_e_recusa(const char *resumo)
{
    static const char *const SINAIS[] = {
        "não posso", "nao posso", "não consigo", "não é possível", "não foi possível", "não incluem", "não inclui",
        "não contém", "não contêm", "não há informa", "não há dados", "não tenho", "não fornece", "não estão disponíveis",
        "não está disponível", "sem informa", "desculpe", "infelizmente", "dados insuficientes", "não há dados suficientes", NULL };
    if (!resumo) return FALSE;
    char *min = g_utf8_strdown(resumo, -1);
    gboolean achou = FALSE;
    for (int i = 0; SINAIS[i] && !achou; i++)
        achou = strstr(min, SINAIS[i]) != NULL;
    g_free(min);
    return achou;
}
