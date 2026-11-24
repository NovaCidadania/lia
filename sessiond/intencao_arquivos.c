/* Pedidos sobre arquivos do projeto -> plano — ver intencao_arquivos.h. */
#include "intencao_arquivos.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

#define L(...) ((const char *const[]){ __VA_ARGS__, NULL })

/* " texto em minúsculas, sem acento, só [a-z0-9/._*+-] e espaços " */
static char *normalizar(const char *s)
{
    char *ascii = g_str_to_ascii(s ? s : "", "C");
    GString *o = g_string_new(" ");
    for (const char *p = ascii; *p; p++) {
        char c = g_ascii_tolower(*p);
        gboolean ok = g_ascii_isalnum(c) || strchr("/._*+-", c);
        g_string_append_c(o, ok ? c : ' ');
    }
    g_string_append_c(o, ' ');
    g_free(ascii);
    return g_string_free(o, FALSE);
}

static gboolean tem(const char *t, const char *const *v)
{
    for (int i = 0; v[i]; i++) if (strstr(t, v[i])) return TRUE;
    return FALSE;
}

/* bloco ```lang\n...\n``` (conteúdo sem a linha da linguagem) */
static char *bloco_de_codigo(const char *pedido, const char **ini, const char **fim)
{
    const char *a = strstr(pedido, "```");
    if (!a) return NULL;
    const char *b = strstr(a + 3, "```");
    if (!b) return NULL;
    const char *c = strchr(a + 3, '\n');
    if (!c || c > b) c = a + 2; /* ```conteúdo``` numa linha só */
    if (ini) *ini = a;
    if (fim) *fim = b + 3;
    return g_strndup(c + 1, (gsize)(b - (c + 1)));
}

typedef struct { const char *abre, *fecha; } Aspas;
static const Aspas ASPAS[] = {
    { "`", "`" }, { "\"", "\"" }, { "\xe2\x80\x9c", "\xe2\x80\x9d" } /* “ ” */, { "\xe2\x80\x98", "\xe2\x80\x99" } /* ‘ ’ */,
    { "\xc2\xab", "\xc2\xbb" } /* « » */, { "'", "'" }, { NULL, NULL },
};

char **intencao_trechos_citados(const char *pedido)
{
    GPtrArray *out = g_ptr_array_new();
    const char *ini_bloco = NULL, *fim_bloco = NULL;
    char *bloco = bloco_de_codigo(pedido, &ini_bloco, &fim_bloco);
    g_free(bloco);
    const char *p = pedido;
    while (p && *p) {
        if (ini_bloco && p >= ini_bloco && p < fim_bloco) { p = fim_bloco; continue; }
        gboolean achou = FALSE;
        for (int i = 0; ASPAS[i].abre; i++) {
            gsize la = strlen(ASPAS[i].abre);
            if (strncmp(p, ASPAS[i].abre, la)) continue;
            /* apóstrofo só conta como aspas no início de palavra ("d'água" não) */
            if (!strcmp(ASPAS[i].abre, "'") && p > pedido && g_ascii_isalnum(p[-1])) continue;
            const char *f = strstr(p + la, ASPAS[i].fecha);
            if (!strcmp(ASPAS[i].abre, "'"))
                while (f && g_ascii_isalnum(f[1])) f = strstr(f + 1, "'");
            if (!f) continue;
            g_ptr_array_add(out, g_strndup(p + la, (gsize)(f - (p + la))));
            p = f + strlen(ASPAS[i].fecha);
            achou = TRUE;
            break;
        }
        if (!achou) p = g_utf8_next_char(p);
    }
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

static gboolean parece_caminho(const char *w)
{
    gsize n = strlen(w);
    if (n == 0 || n > 255 || strstr(w, "://") || strchr(w, ' ') || strchr(w, '\n')) return FALSE;
    gboolean letra = FALSE;
    for (const char *q = w; *q; q++) if (g_ascii_isalpha(*q)) letra = TRUE;
    if (!letra) return FALSE;
    static const char *const SEM_EXTENSAO[] = { "Makefile", "makefile", "Dockerfile", "README", "LICENSE", "COPYING",
                                                "CHANGELOG", "Containerfile", "Jenkinsfile", "Vagrantfile", NULL };
    const char *base = strrchr(w, '/') ? strrchr(w, '/') + 1 : w;
    if (g_strv_contains(SEM_EXTENSAO, base)) return TRUE;
    if (strchr(w, '/')) return TRUE; /* absolutos também: o daemon recusa os de fora do projeto */
    if (w[0] == '.' && n > 1 && g_ascii_isalpha(w[1])) return TRUE; /* .gitignore, .env */
    const char *ponto = strrchr(w, '.');
    if (!ponto || ponto == w || !ponto[1]) return FALSE;
    gsize ext = strlen(ponto + 1);
    if (ext > 10) return FALSE;
    for (const char *q = ponto + 1; *q; q++) if (!g_ascii_isalnum(*q) && *q != '_' && *q != '+' && *q != '-') return FALSE;
    /* "e.g.", "v1.2", "3.14": extensão precisa ter letra */
    gboolean ext_letra = FALSE;
    for (const char *q = ponto + 1; *q; q++) if (g_ascii_isalpha(*q)) ext_letra = TRUE;
    return ext_letra;
}

static char *limpar_palavra(const char *w)
{
    char *c = g_strdup(w);
    gsize n = strlen(c);
    while (n && strchr(",;:!?)]}", c[n - 1])) c[--n] = 0;
    while (n && c[n - 1] == '.' && !(n >= 2 && c[n - 2] == '.')) c[--n] = 0; /* ponto final da frase */
    char *ini = c;
    while (*ini && strchr("([{", *ini)) ini++;
    char *r = g_strdup(ini);
    g_free(c);
    return r;
}

char **intencao_caminhos(const char *pedido)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    char **cit = intencao_trechos_citados(pedido);
    /* tira os trechos citados e o bloco de código do texto livre */
    GString *livre = g_string_new(pedido);
    const char *ib = NULL, *fb = NULL;
    char *bloco = bloco_de_codigo(livre->str, &ib, &fb);
    if (bloco) g_string_erase(livre, ib - livre->str, fb - ib);
    g_free(bloco);
    for (int i = 0; cit[i]; i++) {
        char *pos = strstr(livre->str, cit[i]);
        if (pos && *cit[i]) g_string_erase(livre, pos - livre->str, (gssize)strlen(cit[i]));
    }
    /* na ordem do pedido: citados que parecem caminho e palavras livres */
    GPtrArray *pares = g_ptr_array_new();
    for (int i = 0; cit[i]; i++) {
        if (!parece_caminho(cit[i])) continue;
        const char *pos = strstr(pedido, cit[i]);
        g_ptr_array_add(pares, GSIZE_TO_POINTER((gsize)(pos ? pos - pedido : 0) * 2 + 1));
        g_ptr_array_add(pares, cit[i]);
    }
    char **palavras = g_strsplit_set(livre->str, " \t\n", -1);
    for (int i = 0; palavras[i]; i++) {
        char *w = limpar_palavra(palavras[i]);
        if (parece_caminho(w)) {
            const char *pos = strstr(pedido, w);
            g_ptr_array_add(pares, GSIZE_TO_POINTER((gsize)(pos ? pos - pedido : 0) * 2));
            g_ptr_array_add(pares, w);
        } else {
            g_free(w);
        }
    }
    /* ordena pela posição no pedido (inserção simples; poucos itens) */
    guint n = pares->len / 2;
    for (guint i = 1; i < n; i++)
        for (guint k = i; k > 0 && GPOINTER_TO_SIZE(pares->pdata[2 * k]) < GPOINTER_TO_SIZE(pares->pdata[2 * (k - 1)]); k--) {
            gpointer a = pares->pdata[2 * k], b = pares->pdata[2 * k + 1];
            pares->pdata[2 * k] = pares->pdata[2 * (k - 1)]; pares->pdata[2 * k + 1] = pares->pdata[2 * (k - 1) + 1];
            pares->pdata[2 * (k - 1)] = a; pares->pdata[2 * (k - 1) + 1] = b;
        }
    for (guint i = 0; i < n; i++) {
        const char *c = pares->pdata[2 * i + 1];
        gboolean dup = FALSE;
        for (guint k = 0; k < out->len; k++) if (!strcmp(g_ptr_array_index(out, k), c)) dup = TRUE;
        if (!dup) g_ptr_array_add(out, g_strdup(c));
        if (!(GPOINTER_TO_SIZE(pares->pdata[2 * i]) & 1)) g_free(pares->pdata[2 * i + 1]);
    }
    g_ptr_array_unref(pares);
    g_strfreev(palavras);
    g_string_free(livre, TRUE);
    g_strfreev(cit);
    g_ptr_array_add(out, NULL);
    char **r = (char **)out->pdata;
    g_ptr_array_free(out, FALSE);
    return r;
}

/* texto depois de "com o conteúdo:", "contendo", "com o texto"... no pedido original */
static char *conteudo_apos_marcador(const char *pedido)
{
    static const char *const MARCAS[] = { "com o conteúdo", "com o conteudo", "com conteúdo", "com conteudo", "contendo",
                                          "com o texto", "o texto", "o conteúdo", "o conteudo", NULL };
    char *baixo = g_utf8_strdown(pedido, -1);
    if (strlen(baixo) != strlen(pedido)) { g_free(baixo); return NULL; } /* posições não batem: desiste */
    const char *melhor = NULL;
    gsize tam = 0;
    for (int i = 0; MARCAS[i]; i++) {
        const char *p = strstr(baixo, MARCAS[i]);
        if (p && (!melhor || p < melhor)) { melhor = p; tam = strlen(MARCAS[i]); }
    }
    char *r = NULL;
    if (melhor) {
        const char *q = pedido + (melhor - baixo) + tam;
        while (*q == ' ' || *q == ':') q++;
        if (*q == '\n') q++;
        r = g_strdup(q);
        gsize n = strlen(r);
        /* "…: 'texto'" inteiro entre aspas: tira as aspas */
        for (int i = 0; ASPAS[i].abre && n >= 2; i++) {
            gsize la = strlen(ASPAS[i].abre), lf = strlen(ASPAS[i].fecha);
            /* só quando o texto inteiro é UM trecho entre aspas (a primeira
             * aspas de fechamento é a última) */
            if (n >= la + lf && !strncmp(r, ASPAS[i].abre, la) && strstr(r + la, ASPAS[i].fecha) == r + n - lf) {
                char *x = g_strndup(r + la, n - la - lf);
                g_free(r);
                r = x;
                break;
            }
        }
    }
    g_free(baixo);
    return r;
}

static char *json_params(const char *k1, const char *v1, const char *k2, const char *v2, const char *k3, const char *v3)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    const char *k[] = { k1, k2, k3 }, *v[] = { v1, v2, v3 };
    for (int i = 0; i < 3; i++) {
        if (!k[i] || !v[i]) continue;
        json_builder_set_member_name(b, k[i]);
        json_builder_add_string_value(b, v[i]);
    }
    json_builder_end_object(b);
    JsonGenerator *g = json_generator_new();
    JsonNode *r = json_builder_get_root(b);
    json_generator_set_root(g, r);
    char *s = json_generator_to_data(g, NULL);
    json_node_unref(r);
    g_object_unref(g);
    g_object_unref(b);
    return s;
}

/* acrescenta um membro booleano/inteiro a um objeto JSON em texto */
static char *json_com(char *json, const char *k, const char *valor_json)
{
    gsize n = strlen(json);
    char *r = g_strdup_printf("%.*s%s\"%s\":%s}", (int)(n - 1), json, n > 2 ? "," : "", k, valor_json);
    g_free(json);
    return r;
}

static AgentePlano *plano_um_passo(const char *objetivo, const char *intencao, const char *ferramenta, char *params,
                                   const char *evidencia)
{
    AgentePlano *p = agente_plano_novo(objetivo, intencao);
    agente_plano_acrescentar(p, NULL, ferramenta, params, NULL, objetivo, L(evidencia), 0, FALSE);
    g_free(params);
    p->criterios_sucesso = g_strdupv((char **)L(evidencia));
    return p;
}

static char *codificacao_pedida(const char *pedido, const char *t)
{
    static const struct { const char *chave, *iconv; } NOMES[] = {
        { " utf-8 ", "UTF-8" }, { " utf8 ", "UTF-8" }, { " utf-16le ", "UTF-16LE" }, { " utf-16be ", "UTF-16BE" },
        { " utf-16 ", "UTF-16LE" }, { " utf16 ", "UTF-16LE" }, { " utf-32 ", "UTF-32LE" }, { " ascii ", "US-ASCII" },
        { " latin1 ", "ISO-8859-1" }, { " latin-1 ", "ISO-8859-1" }, { " iso-8859-1 ", "ISO-8859-1" }, { " iso-8859-15 ", "ISO-8859-15" },
        { " windows-1252 ", "WINDOWS-1252" }, { " cp1252 ", "WINDOWS-1252" }, { " windows-1251 ", "WINDOWS-1251" },
        { " cp1251 ", "WINDOWS-1251" }, { " koi8-r ", "KOI8-R" }, { " shift_jis ", "SHIFT_JIS" }, { " shift-jis ", "SHIFT_JIS" },
        { " sjis ", "SHIFT_JIS" }, { " euc-jp ", "EUC-JP" }, { " gb18030 ", "GB18030" }, { " gbk ", "GBK" }, { " gb2312 ", "GB18030" },
        { " big5 ", "BIG5" }, { " euc-kr ", "EUC-KR" }, { " cp437 ", "CP437" }, { " cp850 ", "CP850" }, { " ebcdic ", "EBCDIC-US" },
        { NULL, NULL },
    };
    for (int i = 0; NOMES[i].chave; i++) if (strstr(t, NOMES[i].chave)) return g_strdup(NOMES[i].iconv);
    /* "para <NOME>" com um nome qualquer que o iconv conheça */
    const char *p = strstr(pedido, " para ");
    if (p) {
        char **w = g_strsplit(p + 6, " ", 2);
        char *nome = w[0] ? limpar_palavra(w[0]) : NULL;
        g_strfreev(w);
        if (nome && *nome && strlen(nome) < 40) {
            GIConv cd = g_iconv_open("UTF-8", nome);
            if (cd != (GIConv)-1) { g_iconv_close(cd); char *u = g_ascii_strup(nome, -1); g_free(nome); return u; }
        }
        g_free(nome);
    }
    return NULL;
}

/* "em sessiond", "na pasta src", "dentro de docs": pasta citada só pelo nome */
static char *pasta_apos_preposicao(const char *pedido)
{
    static const char *const VAZIAS[] = { "o", "a", "os", "as", "um", "uma", "todo", "toda", "todos", "todas", "projeto", "código",
                                          "codigo", "arquivo", "arquivos", "repositório", "repositorio", "pasta", "diretório",
                                          "diretorio", "seguida", "vez", "caso", "que", "qual", "quais", NULL };
    GRegex *re = g_regex_new("\\b(?:em|na pasta|no diret[óo]rio|dentro de|dentro da pasta)\\s+([A-Za-z0-9_][A-Za-z0-9_./-]*)",
                             G_REGEX_CASELESS, 0, NULL);
    GMatchInfo *mi = NULL;
    char *r = NULL;
    g_regex_match(re, pedido, 0, &mi);
    while (g_match_info_matches(mi) && !r) {
        char *w = g_match_info_fetch(mi, 1);
        char *l = limpar_palavra(w);
        char *baixo = g_utf8_strdown(l, -1);
        if (*l && !g_strv_contains(VAZIAS, baixo)) r = g_strdup(l);
        g_free(baixo); g_free(l); g_free(w);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(re);
    return r;
}

/* "não rode git", "sem usar o git", "não use git status": o pedido PROÍBE
 * o Git — nenhuma intenção de Git pode sair dele */
gboolean intencao_proibe_git(const char *pedido)
{
    char *t = normalizar(pedido);
    gboolean r = tem(t, L(" nao rode git", " nao rodar git", " nao use git", " nao usar git", " nao use o git", " nao usar o git",
                          " nao execute git", " nao executar git", " nao rode o git", " sem git", " sem usar git", " sem usar o git",
                          " sem rodar git", " sem rodar o git", " nao mexa no git", " nao toque no git", " nada de git", " nao quero git"));
    g_free(t);
    return r;
}

static const char *eol_pedida(const char *t)
{
    if (tem(t, L(" crlf ", " windows ", " dos "))) return "CRLF";
    if (tem(t, L(" cr ", " mac antigo "))) return "CR";
    if (tem(t, L(" lf ", " unix ", " linux "))) return "LF";
    return NULL;
}

AgentePlano *agente_planejar_arquivos(const char *pedido, char **texto_direto)
{
    if (texto_direto) *texto_direto = NULL;
    if (!pedido || !*pedido) return NULL;
    char *t = normalizar(pedido);
    char **cam = intencao_caminhos(pedido);
    char **cit = intencao_trechos_citados(pedido);
    guint ncam = g_strv_length(cam), ncit = g_strv_length(cit);
    gboolean fala_de_arquivo = tem(t, L(" arquivo", " pasta", " diretorio", " projeto", " repositorio", " codigo-fonte", " codigo fonte"));
    gboolean web = tem(t, L(" internet", " web ", " google", " site ", " na rede", " online"));
    AgentePlano *p = NULL;
    const char *falta = NULL;
    char *bloco = bloco_de_codigo(pedido, NULL, NULL);

    if (web) goto fim;

    /* ---- validação: build e testes (sempre com aprovação, em isolamento) ---- */
    gboolean de_codigo = fala_de_arquivo || ncam > 0 || tem(t, L(" codigo", " programa", " make ", " build", " teste"));
    if (tem(t, L(" rode os testes", " rodar os testes", " roda os testes", " execute os testes", " executar os testes", " rode o teste",
                 " rode a validacao", " rodar a validacao", " valide o projeto", " compile o projeto", " compilar o projeto",
                 " rode o build", " rodar o build", " faca o build", " make teste", " make test", " rode o make", " passa nos testes")) ||
        (tem(t, L(" compile ", " compilar ")) && de_codigo)) {
        const char *cmd = tem(t, L(" compile", " compilar", " build", " rode o make ")) && !tem(t, L(" teste")) ? "build" : NULL;
        /* nome explícito entre aspas: rode a validação "lint" */
        if (ncit >= 1) cmd = cit[0];
        char *pj = json_params("comando", cmd, NULL, NULL, NULL, NULL);
        char *obj = g_strdup_printf("rodar a validação %s%s%s do projeto em isolamento", cmd ? "\"" : "padrão", cmd ? cmd : "", cmd ? "\"" : "");
        p = plano_um_passo(obj, "validar", "validar.executar", pj, "validacao_executada");
        g_free(obj);
        goto fim;
    }
    if (tem(t, L(" comandos de validacao", " como rodar os testes", " como compilar", " quais testes", " comandos de teste", " comandos de build"))) {
        p = plano_um_passo("listar os comandos de validação do projeto", "validar", "validar.listar", g_strdup("{}"), "validacao_listada");
        goto fim;
    }

    /* ---- Git: consultas e commit (nunca quando o pedido proíbe o Git) ---- */
    if (intencao_proibe_git(pedido)) goto sem_git;
    gboolean fala_git = tem(t, L(" git ", " commit", " commite", " ramo", " branch", " historico", " repositorio"));
    if (tem(t, L(" faca commit", " faca um commit", " fazer commit", " fazer um commit", " commite ", " commitar ", " de commit", " git commit"))) {
        const char *msg = NULL;
        /* a mensagem é o trecho citado que não é caminho */
        for (guint i = 0; i < ncit && !msg; i++) {
            gboolean e_caminho = FALSE;
            for (guint k = 0; k < ncam; k++) if (!strcmp(cam[k], cit[i])) e_caminho = TRUE;
            if (!e_caminho) msg = cit[i];
        }
        if (!msg) { falta = "Qual a mensagem do commit? Por exemplo: faça commit de src/main.c com a mensagem \"Corrige o cálculo\". Sem lista de arquivos, entram todos os alterados (mostrados antes de você aprovar)."; goto fim; }
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "mensagem");
        json_builder_add_string_value(b, msg);
        guint n_arq = 0;
        for (guint k = 0; k < ncam; k++) if (strcmp(cam[k], msg)) n_arq++;
        if (n_arq) {
            json_builder_set_member_name(b, "arquivos");
            json_builder_begin_array(b);
            for (guint k = 0; k < ncam; k++) if (strcmp(cam[k], msg)) json_builder_add_string_value(b, cam[k]);
            json_builder_end_array(b);
        }
        json_builder_end_object(b);
        JsonGenerator *g = json_generator_new();
        JsonNode *raiz = json_builder_get_root(b);
        json_generator_set_root(g, raiz);
        char *pj = json_generator_to_data(g, NULL);
        json_node_unref(raiz);
        g_object_unref(g);
        g_object_unref(b);
        p = plano_um_passo("fazer um commit local (sem push) com os arquivos mostrados na prévia", "git_commit", "git.commit", pj, "commit_criado");
        goto fim;
    }
    if (tem(t, L(" git status", " status do git", " status do repositorio", " arquivos alterados", " arquivos modificados",
                 " em que ramo", " qual ramo", " qual branch")) ||
        ((fala_git || fala_de_arquivo) && tem(t, L(" o que mudou", " o que foi alterado")) && !ncam)) {
        p = plano_um_passo("ver o status do Git", "git_ler", "git.status", g_strdup("{}"), "git_status");
        goto fim;
    }
    if (tem(t, L(" git diff", " mostre o diff", " mostrar o diff", " ver o diff", " diff do", " diff de", " alteracoes nao commitadas")) ||
        (tem(t, L(" o que mudou em", " o que foi alterado em")) && ncam)) {
        char *pj = json_params("caminho", ncam ? cam[0] : NULL, NULL, NULL, NULL, NULL);
        if (tem(t, L(" preparad", " staged", " cached", " no indice"))) pj = json_com(pj, "preparado", "true");
        p = plano_um_passo("ver as alterações (git diff)", "git_ler", "git.diff", pj, "git_diff");
        goto fim;
    }
    if (tem(t, L(" git log", " historico de commits", " historico do git", " ultimos commits", " ultimo commit", " commits recentes")) ||
        (fala_git && tem(t, L(" historico")))) {
        int n = 10;
        GRegex *re = g_regex_new("(\\d+)\\s+(?:últimos|ultimos)?\\s*commits", G_REGEX_CASELESS, 0, NULL);
        GMatchInfo *mi = NULL;
        if (g_regex_match(re, pedido, 0, &mi)) { char *x = g_match_info_fetch(mi, 1); n = atoi(x); g_free(x); }
        g_match_info_free(mi);
        g_regex_unref(re);
        if (tem(t, L(" ultimo commit"))) n = 1;
        if (n < 1) n = 1;
        if (n > 100) n = 100;
        char lim[16];
        g_snprintf(lim, sizeof lim, "%d", n);
        p = plano_um_passo("ver os últimos commits (git log)", "git_ler", "git.log", json_com(g_strdup("{}"), "limite", lim), "git_log");
        goto fim;
    }
sem_git:

    /* ---- desfazer uma operação anterior ---- */
    if (tem(t, L(" desfaca ", " desfazer ", " desfaz ", " reverta a operacao", " reverter a operacao"))) {
        GRegex *re = g_regex_new("\\b(\\d{8}-\\d{6}-[0-9a-f]{6})\\b", 0, 0, NULL);
        GMatchInfo *mi = NULL;
        if (g_regex_match(re, pedido, 0, &mi)) {
            char *id = g_match_info_fetch(mi, 1);
            p = plano_um_passo("desfazer uma alteração anterior de arquivos", "arquivo_alterar", "arquivo.desfazer",
                               json_params("operacao_id", id, NULL, NULL, NULL, NULL), "arquivo_alterado");
            g_free(id);
        } else if (fala_de_arquivo || strstr(t, " operacao")) {
            falta = "Qual operação desfazer? Informe o identificador mostrado quando a alteração foi feita (por exemplo, 20261123-204201-a1b2c3).";
        }
        g_match_info_free(mi);
        g_regex_unref(re);
        goto fim;
    }

    /* ---- substituir um trecho: "substitua 'a' por 'b' em x.c" ---- */
    if (tem(t, L(" substitua ", " substituir ", " troque ", " trocar ", " altere ", " alterar ", " mude ", " mudar ", " edite ", " editar ")) &&
        strstr(t, " por ") && ncit >= 2) {
        const char *alvo = NULL;
        for (guint i = 0; i < ncam && !alvo; i++)
            if (strcmp(cam[i], cit[0]) && strcmp(cam[i], cit[1])) alvo = cam[i];
        if (!alvo && ncit >= 3 && parece_caminho(cit[2])) alvo = cit[2];
        if (!alvo) { falta = "Em qual arquivo devo fazer a substituição? Diga o caminho (por exemplo, src/main.c)."; goto fim; }
        char *pj = json_params("caminho", alvo, "procurar", cit[0], "substituir", cit[1]);
        if (tem(t, L(" todas ", " todos ", " em todo ", " toda ocorrencia", " todas as ocorrencias"))) pj = json_com(pj, "todas", "true");
        char *obj = g_strdup_printf("substituir um trecho em %s", alvo);
        p = plano_um_passo(obj, "arquivo_alterar", "arquivo.editar", pj, "arquivo_alterado");
        g_free(obj);
        goto fim;
    }

    /* ---- mover/renomear e copiar: dois caminhos ---- */
    gboolean mover = tem(t, L(" renomeie ", " renomear ", " mova ", " mover ", " mv "));
    gboolean copiar = tem(t, L(" copie ", " copiar ", " duplique ", " duplicar ", " cp "));
    if (mover || copiar) {
        if (ncam >= 2) {
            char *obj = g_strdup_printf("%s %s para %s", mover ? "mover/renomear" : "copiar", cam[0], cam[1]);
            p = plano_um_passo(obj, "arquivo_alterar", mover ? "arquivo.mover" : "arquivo.copiar",
                               json_params("origem", cam[0], "destino", cam[1], NULL, NULL), "arquivo_alterado");
            g_free(obj);
        } else if (fala_de_arquivo || ncam == 1) {
            falta = mover ? "Para mover/renomear preciso da origem e do destino (por exemplo: renomeie src/a.c para src/b.c)."
                          : "Para copiar preciso da origem e do destino (por exemplo: copie a.txt para b.txt).";
        }
        goto fim;
    }

    /* ---- apagar ---- */
    if (tem(t, L(" apague ", " apagar ", " delete ", " deletar ", " remova ", " remover ", " exclua ", " excluir ", " rm "))) {
        if (ncam >= 1) {
            char *obj = g_strdup_printf("apagar %s (fica uma cópia para desfazer)", cam[0]);
            p = plano_um_passo(obj, "arquivo_alterar", "arquivo.apagar", json_params("caminho", cam[0], NULL, NULL, NULL, NULL), "arquivo_alterado");
            g_free(obj);
            if (ncam > 1) {
                guint n = p->limitacoes ? g_strv_length(p->limitacoes) : 0;
                p->limitacoes = g_realloc(p->limitacoes, sizeof(char *) * (n + 2));
                p->limitacoes[n] = g_strdup("apago um arquivo por vez: peça os outros em seguida (nada é apagado em massa)");
                p->limitacoes[n + 1] = NULL;
            }
        } else if (fala_de_arquivo) {
            falta = "Qual arquivo devo apagar? Diga o caminho dentro do projeto.";
        }
        goto fim;
    }

    /* ---- converter codificação / quebra de linha ---- */
    if ((tem(t, L(" converta ", " converter ", " mude ", " altere ", " troque ", " passe ")) &&
         tem(t, L(" codificacao", " encoding", " quebra de linha", " quebras de linha", " final de linha", " crlf ", " utf", " latin", " windows-", " iso-8859", " charset"))) ||
        tem(t, L(" converta ", " converter ")) ) {
        if (ncam >= 1) {
            char *cod = codificacao_pedida(pedido, t);
            const char *eol = tem(t, L(" quebra", " final de linha", " crlf ", " lf ", " cr ")) ? eol_pedida(t) : NULL;
            if (!cod && !eol) { falta = "Converter para qual codificação (UTF-8, WINDOWS-1252, UTF-16...) ou quebra de linha (LF, CRLF, CR)?"; goto fim; }
            char *pj = json_params("caminho", cam[0], "codificacao", cod, "quebra_de_linha", eol);
            char *obj = g_strdup_printf("converter %s%s%s%s%s", cam[0], cod ? " para " : "", cod ? cod : "", eol ? " com quebra de linha " : "", eol ? eol : "");
            p = plano_um_passo(obj, "arquivo_alterar", "arquivo.converter", pj, "arquivo_alterado");
            g_free(obj);
            g_free(cod);
            goto fim;
        }
        if (fala_de_arquivo) { falta = "Qual arquivo devo converter?"; goto fim; }
    }

    gboolean criar = tem(t, L(" crie ", " criar ", " gere ", " gerar ", " escreva ", " escrever ", " novo arquivo", " faca um arquivo", " mkdir ", " touch "));

    /* ---- permissão de execução ("crie run.sh executável" é criação) ---- */
    if (!criar && tem(t, L(" executavel", " chmod ", " permissao de execucao", " permissao para executar"))) {
        if (ncam >= 1) {
            gboolean tirar = tem(t, L(" nao executavel", " tire ", " remova a permissao", " tirar ", " sem permissao", " chmod -x"));
            char *pj = json_com(json_params("caminho", cam[0], NULL, NULL, NULL, NULL), "executavel", tirar ? "false" : "true");
            char *obj = g_strdup_printf("%s %s", tirar ? "tirar a permissão de execução de" : "tornar executável", cam[0]);
            p = plano_um_passo(obj, "arquivo_alterar", "arquivo.permissoes", pj, "arquivo_alterado");
            g_free(obj);
        } else if (fala_de_arquivo) {
            falta = "Qual arquivo devo tornar executável?";
        }
        goto fim;
    }

    /* ---- criar pasta ---- */
    if (criar && tem(t, L(" pasta", " diretorio", " mkdir "))) {
        const char *alvo = ncam ? cam[0] : NULL;
        /* "crie a pasta testes" — a palavra depois de pasta/diretório */
        char *depois = NULL;
        if (!alvo) {
            const char *q = NULL;
            for (const char *const *k = L("pasta ", "diretório ", "diretorio ", "mkdir "); *k && !q; k++) q = strstr(pedido, *k);
            if (q) {
                q = strchr(q, ' ') + 1;
                while (*q == ' ') q++;
                static const char *const ART[] = { "chamada ", "chamado ", "com o nome ", "de nome ", NULL };
                for (int i = 0; ART[i]; i++) if (!strncmp(q, ART[i], strlen(ART[i]))) q += strlen(ART[i]);
                char **w = g_strsplit(q, " ", 2);
                if (w[0]) depois = limpar_palavra(w[0]);
                g_strfreev(w);
                if (depois && (!*depois || !g_ascii_isalnum(depois[0]))) g_clear_pointer(&depois, g_free);
            }
            alvo = depois;
        }
        if (alvo) {
            char *obj = g_strdup_printf("criar a pasta %s", alvo);
            p = plano_um_passo(obj, "arquivo_alterar", "arquivo.criar_pasta", json_params("caminho", alvo, NULL, NULL, NULL, NULL), "arquivo_alterado");
            g_free(obj);
        } else {
            falta = "Qual o nome (caminho) da pasta a criar?";
        }
        g_free(depois);
        goto fim;
    }

    /* conteúdo para criar/escrever/acrescentar */
    char *conteudo = bloco ? g_strdup(bloco) : conteudo_apos_marcador(pedido);
    if (!conteudo)
        for (guint i = 0; i < ncit && !conteudo; i++)
            if (!ncam || strcmp(cit[i], cam[0])) conteudo = g_strdup(cit[i]);

    /* ---- acrescentar ao fim ---- */
    if (tem(t, L(" acrescente ", " acrescentar ", " anexe ", " anexar ", " adicione ", " adicionar ", " inclua ", " incluir ", " append ")) &&
        ncam >= 1 && (tem(t, L(" final", " fim ")) || conteudo)) {
        if (!conteudo) { falta = "O que devo acrescentar? Coloque o texto entre aspas ou num bloco ```."; g_free(conteudo); goto fim; }
        char *obj = g_strdup_printf("acrescentar texto ao fim de %s", cam[0]);
        p = plano_um_passo(obj, "arquivo_alterar", "arquivo.anexar", json_params("caminho", cam[0], "conteudo", conteudo, NULL, NULL), "arquivo_alterado");
        g_free(obj);
        g_free(conteudo);
        goto fim;
    }

    /* ---- sobrescrever ---- */
    if (tem(t, L(" sobrescreva ", " sobrescrever ", " reescreva ", " reescrever ", " substitua o conteudo", " substituir o conteudo", " troque o conteudo"))) {
        if (ncam >= 1 && conteudo) {
            char *obj = g_strdup_printf("substituir o conteúdo de %s", cam[0]);
            p = plano_um_passo(obj, "arquivo_alterar", "arquivo.escrever", json_params("caminho", cam[0], "conteudo", conteudo, NULL, NULL), "arquivo_alterado");
            g_free(obj);
        } else {
            falta = ncam ? "Qual o novo conteúdo? Coloque-o entre aspas ou num bloco ```." : "Qual arquivo devo reescrever?";
        }
        g_free(conteudo);
        goto fim;
    }

    /* ---- criar arquivo ---- */
    if (criar && (ncam >= 1 || fala_de_arquivo)) {
        if (!ncam) { falta = "Qual o nome (caminho) do arquivo a criar? Por exemplo: crie o arquivo src/ola.c com o conteúdo ```...```"; g_free(conteudo); goto fim; }
        gboolean vazio = tem(t, L(" vazio ", " em branco ", " touch "));
        if (!conteudo && !vazio) {
            falta = NULL; /* sem conteúdo literal: é geração de código (pelo modelo), não deste planejador */
            g_free(conteudo);
            goto fim;
        }
        char *obj = g_strdup_printf("criar o arquivo %s", cam[0]);
        char *pj = json_params("caminho", cam[0], "conteudo", conteudo ? conteudo : "", NULL, NULL);
        if (tem(t, L(" executavel"))) pj = json_com(pj, "executavel", "true");
        p = plano_um_passo(obj, "arquivo_alterar", "arquivo.criar", pj, "arquivo_alterado");
        g_free(obj);
        g_free(conteudo);
        goto fim;
    }
    g_free(conteudo);

    /* ---- procurar um texto nos arquivos ---- */
    if (tem(t, L(" procure ", " procurar ", " busque ", " buscar ", " encontre ", " encontrar ", " pesquise ", " grep ", " onde ", " ocorrencias "))) {
        char *pasta_dita = pasta_apos_preposicao(pedido);
        gboolean no_projeto = fala_de_arquivo || tem(t, L(" no codigo", " nos fontes", " em todo", " no src", " nos arquivos")) || ncam > 0 || pasta_dita;
        g_free(pasta_dita);
        const char *texto = NULL;
        for (guint i = 0; i < ncit && !texto; i++) if (!ncam || strcmp(cit[i], cam[0])) texto = cit[i];
        if (texto && no_projeto) {
            const char *onde = NULL, *padrao = NULL;
            for (guint i = 0; i < ncam; i++) {
                if (!strcmp(cam[i], texto)) continue;
                if (cam[i][0] == '*') padrao = cam[i]; else if (!onde) onde = cam[i];
            }
            char *pasta = onde ? NULL : pasta_apos_preposicao(pedido);
            if (pasta && !strcmp(pasta, texto)) g_clear_pointer(&pasta, g_free);
            if (pasta) onde = pasta;
            char *pj = json_params("texto", texto, "caminho", onde, "padrao", padrao);
            if (tem(t, L(" regex", " expressao regular"))) pj = json_com(pj, "regex", "true");
            if (tem(t, L(" maiusculas e minusculas", " case sensitive", " exatamente assim"))) pj = json_com(pj, "diferenciar_maiusculas", "true");
            char *obj = g_strdup_printf("procurar \"%s\" nos arquivos do projeto", texto);
            p = plano_um_passo(obj, "arquivo_buscar", "arquivo.buscar", pj, "busca_arquivos");
            g_free(obj);
            g_free(pasta);
            goto fim;
        }
        if (no_projeto && !texto && fala_de_arquivo && !ncam) {
            falta = "O que devo procurar nos arquivos? Coloque o texto entre aspas, por exemplo: procure \"TODO\" no projeto.";
            goto fim;
        }
    }

    /* ---- informações do arquivo (codificação, tamanho...) ---- */
    if (ncam >= 1 && tem(t, L(" codificacao", " encoding", " charset", " tamanho", " informacoes", " info ", " detalhes do arquivo",
                               " quebra de linha", " permissoes", " sha256", " checksum", " e binario", " tipo do arquivo"))) {
        char *obj = g_strdup_printf("informações de %s", cam[0]);
        p = plano_um_passo(obj, "arquivo_info", "arquivo.info", json_params("caminho", cam[0], NULL, NULL, NULL, NULL), "info_arquivo");
        g_free(obj);
        goto fim;
    }

    /* ---- listar ---- */
    if (tem(t, L(" liste ", " listar ", " lista ", " quais arquivos", " que arquivos", " mostre os arquivos", " mostrar os arquivos",
                 " arquivos do projeto", " estrutura do projeto", " arvore", " ls ", " o que tem na pasta", " o que ha na pasta", " conteudo da pasta")) &&
        (fala_de_arquivo || ncam || tem(t, L(" ls ")))) {
        const char *onde = NULL, *padrao = NULL;
        for (guint i = 0; i < ncam; i++) { if (strchr(cam[i], '*')) padrao = cam[i]; else if (!onde) onde = cam[i]; }
        /* "arquivos .c" / "arquivos .py" */
        char *pad_ext = NULL;
        if (!padrao) {
            GRegex *re = g_regex_new("arquivos\\s+(\\.[A-Za-z0-9]{1,8})\\b", G_REGEX_CASELESS, 0, NULL);
            GMatchInfo *mi = NULL;
            if (g_regex_match(re, pedido, 0, &mi)) { char *e = g_match_info_fetch(mi, 1); pad_ext = g_strconcat("*", e, NULL); g_free(e); padrao = pad_ext; }
            g_match_info_free(mi);
            g_regex_unref(re);
            if (padrao && onde && g_str_has_suffix(onde, padrao + 1) && !strchr(onde, '/')) onde = NULL;
        }
        char *pasta = onde ? NULL : pasta_apos_preposicao(pedido);
        if (pasta) onde = pasta;
        char *pj = json_params("caminho", onde, "padrao", padrao, NULL, NULL);
        if (tem(t, L(" todos ", " todas ", " recursiv", " estrutura", " arvore", " subpastas", " inteiro")) || padrao) pj = json_com(pj, "recursivo", "true");
        if (tem(t, L(" ocultos", " escondidos"))) pj = json_com(pj, "ocultos", "true");
        char *obj = g_strdup_printf("listar arquivos%s%s", onde ? " em " : " do projeto", onde ? onde : "");
        p = plano_um_passo(obj, "arquivo_listar", "arquivo.listar", pj, "lista_arquivos");
        g_free(obj);
        g_free(pad_ext);
        g_free(pasta);
        goto fim;
    }

    /* ---- ler/mostrar ---- */
    if (ncam >= 1 && tem(t, L(" leia ", " ler ", " mostre ", " mostrar ", " exiba ", " exibir ", " abra ", " abrir ", " veja ", " ver ", " cat ",
                               " conteudo ", " o que tem ", " o que ha ", " o que diz ", " imprima ", " print "))) {
        char *pj = json_params("caminho", cam[0], NULL, NULL, NULL, NULL);
        GRegex *re = g_regex_new("linhas?\\s+(\\d+)(?:\\s*(?:a|até|ate|-|–)\\s*(\\d+))?", G_REGEX_CASELESS, 0, NULL);
        GMatchInfo *mi = NULL;
        if (g_regex_match(re, pedido, 0, &mi)) {
            char *a = g_match_info_fetch(mi, 1), *b = g_match_info_fetch(mi, 2);
            gint64 ini = g_ascii_strtoll(a, NULL, 10), fim_l = b && *b ? g_ascii_strtoll(b, NULL, 10) : 0;
            if (ini >= 1) {
                pj = json_com(pj, "linha_inicial", a);
                if (fim_l >= ini) { char *n = g_strdup_printf("%" G_GINT64_FORMAT, MIN(fim_l - ini + 1, 2000)); pj = json_com(pj, "linhas", n); g_free(n); }
            }
            g_free(a); g_free(b);
        }
        g_match_info_free(mi);
        g_regex_unref(re);
        char *cod = tem(t, L(" como ", " em ", " codificacao ")) ? codificacao_pedida(pedido, t) : NULL;
        if (cod) {
            char *q = g_strdup_printf("\"%s\"", cod); /* nome validado pelo iconv: só [A-Z0-9_-] */
            pj = json_com(pj, "codificacao", q);
            g_free(q);
        }
        g_free(cod);
        char *obj = g_strdup_printf("ler %s", cam[0]);
        p = plano_um_passo(obj, "arquivo_ler", "arquivo.ler", pj, "conteudo_arquivo");
        g_free(obj);
        goto fim;
    }

fim:
    if (!p && falta && texto_direto) *texto_direto = g_strdup(falta);
    g_free(bloco);
    g_strfreev(cam);
    g_strfreev(cit);
    g_free(t);
    return p;
}
