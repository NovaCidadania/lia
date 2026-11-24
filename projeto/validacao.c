/* Validação do projeto — ver projeto/validacao.h. */
#define _GNU_SOURCE
#include "validacao.h"
#include "arquivos.h"
#include "processo.h"

#include <string.h>
#include <sys/stat.h>

typedef struct {
    char *nome;
    char **argv;
    int timeout_s;
} Comando;

static void comando_liberar(gpointer p)
{
    Comando *c = p;
    g_free(c->nome);
    g_strfreev(c->argv);
    g_free(c);
}

/* executáveis que nunca rodam como validação: shells e "embrulhos" que
 * executam outro comando qualquer (a composição de comandos fica proibida) */
static const char *const PROIBIDOS[] = {
    "sh", "bash", "dash", "zsh", "fish", "ksh", "mksh", "csh", "tcsh", "busybox", "toybox", "env", "xargs", "nohup", "sudo",
    "doas", "pkexec", "su", "runuser", "timeout", "nice", "ionice", "setsid", "chroot", "unshare", "bwrap", "strace", "ltrace",
    "gdb", "ssh", "scp", "rsync", "curl", "wget", "nc", "ncat", "socat", "eval", "exec", "watch", "flock", "stdbuf", "script",
    "expect", "find", "awk", "gawk", "sed", "perl", "ruby", "lua", "php", "tclsh", "osascript", NULL,
};

/* padrão; a pessoa pode trocar com LIA_SESSIOND_VALIDACAO_PERMITIDOS (vírgulas) */
static const char *const PERMITIDOS_PADRAO =
    "make,gmake,gcc,cc,c99,clang,tcc,g++,c++,clang++,python3,python,pytest,cargo,rustc,go,node,npm,meson,ninja,cmake,ctest,"
    "zig,ghc,cabal,stack,dotnet,java,javac,mvn,gradle,valgrind";

/* interpretadores: código em linha equivale a um shell */
static gboolean codigo_em_linha(const char *base, char **argv)
{
    for (int i = 1; argv[i]; i++) {
        const char *a = argv[i];
        if (g_str_has_prefix(base, "python") && (!strcmp(a, "-c") || (a[0] == '-' && a[1] != '-' && strchr(a, 'c')))) return TRUE;
        if (!strcmp(base, "node") && (!strcmp(a, "-e") || !strcmp(a, "-p") || !strcmp(a, "--eval") || !strcmp(a, "--print"))) return TRUE;
    }
    return FALSE;
}

gboolean validacao_argv_permitido(char **argv, char **motivo)
{
    if (!argv || !argv[0] || !*argv[0]) { *motivo = g_strdup("comando vazio"); return FALSE; }
    char *base = g_path_get_basename(argv[0]);
    gboolean ok = TRUE;
    if (g_strv_contains(PROIBIDOS, base)) {
        *motivo = g_strdup_printf("'%s' é um shell ou executa outros comandos — proibido como validação (use um alvo do make ou um script chamado pelo make)", base);
        ok = FALSE;
    } else if (codigo_em_linha(base, argv)) {
        *motivo = g_strdup_printf("código em linha ('%s -c'/'-e') não é aceito — coloque o código num arquivo do projeto", base);
        ok = FALSE;
    } else if (!strchr(argv[0], '/')) {
        const char *lista = g_getenv("LIA_SESSIOND_VALIDACAO_PERMITIDOS");
        char **perm = g_strsplit(lista && *lista ? lista : PERMITIDOS_PADRAO, ",", -1);
        if (!g_strv_contains((const char *const *)perm, base)) {
            *motivo = g_strdup_printf("'%s' não está na lista de executáveis permitidos para validação (LIA_SESSIOND_VALIDACAO_PERMITIDOS)", base);
            ok = FALSE;
        }
        g_strfreev(perm);
    }
    for (int i = 0; ok && argv[i]; i++)
        for (const char *q = argv[i]; *q; q++)
            if ((unsigned char)*q < 0x20 && *q != '\t') { *motivo = g_strdup("argumento com caractere de controle"); ok = FALSE; break; }
    g_free(base);
    return ok;
}

static gboolean nome_valido(const char *n)
{
    if (!n || !*n || strlen(n) > 32) return FALSE;
    for (const char *q = n; *q; q++) if (!g_ascii_isalnum(*q) && *q != '-' && *q != '_') return FALSE;
    return TRUE;
}

static char **argv_de_json(JsonNode *n, char **erro, const char *nome)
{
    if (JSON_NODE_TYPE(n) != JSON_NODE_ARRAY) { *erro = g_strdup_printf("comando '%s': argv precisa ser uma lista de textos", nome); return NULL; }
    JsonArray *a = json_node_get_array(n);
    guint k = json_array_get_length(a);
    if (k == 0 || k > 64) { *erro = g_strdup_printf("comando '%s': argv precisa ter de 1 a 64 itens", nome); return NULL; }
    char **v = g_new0(char *, k + 1);
    for (guint i = 0; i < k; i++) {
        JsonNode *e = json_array_get_element(a, i);
        if (JSON_NODE_TYPE(e) != JSON_NODE_VALUE || json_node_get_value_type(e) != G_TYPE_STRING ||
            strlen(json_node_get_string(e)) > 512 || !g_utf8_validate(json_node_get_string(e), -1, NULL)) {
            *erro = g_strdup_printf("comando '%s': item %u do argv inválido", nome, i + 1);
            g_strfreev(v);
            return NULL;
        }
        v[i] = g_strdup(json_node_get_string(e));
    }
    return v;
}

/* comandos do projeto: (lista, origem, sha256 da configuração) */
static GPtrArray *carregar(const char **origem, char **sha_config, char **erro)
{
    const char *raiz = proj_raiz();
    if (!raiz) { *erro = g_strdup("nenhum projeto definido (LIA_SESSIOND_PROJETO)"); return NULL; }
    GPtrArray *cmds = g_ptr_array_new_with_free_func(comando_liberar);
    char *arq = g_build_filename(raiz, ".lia", "validacao.json", NULL);
    char *dados = NULL;
    gsize n = 0;
    struct stat st;
    if (lstat(arq, &st) == 0 && !S_ISREG(st.st_mode)) {
        *erro = g_strdup(".lia/validacao.json não é um arquivo comum (link simbólico recusado)");
        g_free(arq); g_ptr_array_unref(cmds);
        return NULL;
    }
    if (g_file_get_contents(arq, &dados, &n, NULL)) {
        *origem = ".lia/validacao.json (configuração do projeto — dado não confiável; quem autoriza é a sua aprovação)";
        if (sha_config) *sha_config = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)dados, n);
        JsonParser *jp = json_parser_new();
        if (n > 64 * 1024 || !json_parser_load_from_data(jp, dados, (gssize)n, NULL) ||
            JSON_NODE_TYPE(json_parser_get_root(jp)) != JSON_NODE_OBJECT) {
            *erro = g_strdup(".lia/validacao.json inválido (precisa ser um objeto JSON de até 64 KiB)");
            g_object_unref(jp); g_free(dados); g_free(arq); g_ptr_array_unref(cmds);
            return NULL;
        }
        JsonObject *raizj = json_node_get_object(json_parser_get_root(jp));
        JsonObject *co = json_object_has_member(raizj, "comandos") && JSON_NODE_TYPE(json_object_get_member(raizj, "comandos")) == JSON_NODE_OBJECT
                             ? json_object_get_object_member(raizj, "comandos") : NULL;
        if (!co) {
            *erro = g_strdup(".lia/validacao.json: falta o objeto \"comandos\"");
            g_object_unref(jp); g_free(dados); g_free(arq); g_ptr_array_unref(cmds);
            return NULL;
        }
        GList *nomes = json_object_get_members(co);
        nomes = g_list_sort(nomes, (GCompareFunc)strcmp);
        for (GList *it = nomes; it; it = it->next) {
            const char *nome = it->data;
            if (!nome_valido(nome)) { *erro = g_strdup_printf(".lia/validacao.json: nome de comando inválido: %s", nome); break; }
            JsonNode *v = json_object_get_member(co, nome);
            JsonNode *argv_n = v;
            int timeout = VALIDACAO_TIMEOUT_PADRAO_S;
            if (JSON_NODE_TYPE(v) == JSON_NODE_OBJECT) {
                JsonObject *o = json_node_get_object(v);
                argv_n = json_object_has_member(o, "argv") ? json_object_get_member(o, "argv") : NULL;
                if (json_object_has_member(o, "timeout_segundos")) timeout = (int)json_object_get_int_member(o, "timeout_segundos");
            }
            char **argv = argv_n ? argv_de_json(argv_n, erro, nome) : NULL;
            if (!argv) { if (!*erro) *erro = g_strdup_printf("comando '%s' sem argv", nome); break; }
            if (timeout < 1) timeout = 1;
            if (timeout > VALIDACAO_TIMEOUT_MAX_S) timeout = VALIDACAO_TIMEOUT_MAX_S;
            Comando *c = g_new0(Comando, 1);
            c->nome = g_strdup(nome);
            c->argv = argv;
            c->timeout_s = timeout;
            g_ptr_array_add(cmds, c);
        }
        g_list_free(nomes);
        g_object_unref(jp);
        g_free(dados);
        g_free(arq);
        if (*erro) { g_ptr_array_unref(cmds); return NULL; }
        return cmds;
    }
    g_free(arq);
    if (sha_config) *sha_config = g_strdup("sem-configuracao");
    /* detecção: Makefile */
    char *mk = g_build_filename(raiz, "Makefile", NULL);
    char *mkdados = NULL;
    if (g_file_get_contents(mk, &mkdados, NULL, NULL)) {
        *origem = "detectado no Makefile (não há .lia/validacao.json)";
        Comando *b = g_new0(Comando, 1);
        b->nome = g_strdup("build");
        b->argv = g_strdupv((char *[]){ (char *)"make", NULL });
        b->timeout_s = VALIDACAO_TIMEOUT_PADRAO_S;
        g_ptr_array_add(cmds, b);
        static const char *const ALVOS[] = { "teste", "test", "check", NULL };
        for (int i = 0; ALVOS[i]; i++) {
            char *re_txt = g_strdup_printf("^%s\\s*:", ALVOS[i]);
            GRegex *re = g_regex_new(re_txt, G_REGEX_MULTILINE, 0, NULL);
            if (g_regex_match(re, mkdados, 0, NULL)) {
                Comando *t = g_new0(Comando, 1);
                t->nome = g_strdup(ALVOS[i]);
                t->argv = g_strdupv((char *[]){ (char *)"make", (char *)ALVOS[i], NULL });
                t->timeout_s = VALIDACAO_TIMEOUT_PADRAO_S;
                g_ptr_array_add(cmds, t);
            }
            g_regex_unref(re);
            g_free(re_txt);
        }
        g_free(mkdados);
    } else {
        *origem = "nenhum (crie .lia/validacao.json ou um Makefile)";
    }
    g_free(mk);
    return cmds;
}

static Comando *achar(GPtrArray *cmds, const char *nome)
{
    for (guint i = 0; i < cmds->len; i++) if (!g_strcmp0(((Comando *)g_ptr_array_index(cmds, i))->nome, nome)) return g_ptr_array_index(cmds, i);
    return NULL;
}

char *validacao_comando_padrao(void)
{
    const char *origem = NULL;
    char *erro = NULL;
    GPtrArray *cmds = carregar(&origem, NULL, &erro);
    g_free(erro);
    if (!cmds) return NULL;
    char *r = NULL;
    static const char *const PREF[] = { "teste", "test", "testes", "check", "build", NULL };
    for (int i = 0; PREF[i] && !r; i++) if (achar(cmds, PREF[i])) r = g_strdup(PREF[i]);
    if (!r && cmds->len) r = g_strdup(((Comando *)g_ptr_array_index(cmds, 0))->nome);
    g_ptr_array_unref(cmds);
    return r;
}

static void juntar_entradas(const char *raiz, const char *abs, GPtrArray *linhas, guint *n)
{
    GDir *d = g_dir_open(abs, 0, NULL);
    if (!d) return;
    const char *nm;
    while ((nm = g_dir_read_name(d)) && *n < 100000) {
        if (!strcmp(nm, ".git") && !strcmp(abs, raiz)) continue;
        char *f = g_build_filename(abs, nm, NULL);
        struct stat st;
        if (lstat(f, &st) == 0) {
            (*n)++;
            g_ptr_array_add(linhas, g_strdup_printf("%s|%lld|%lld.%09ld|%o", f + strlen(raiz), (long long)st.st_size,
                                                    (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec, (unsigned)st.st_mode));
            if (S_ISDIR(st.st_mode)) juntar_entradas(raiz, f, linhas, n);
        }
        g_free(f);
    }
    g_dir_close(d);
}

static gint cmp_txt(gconstpointer a, gconstpointer b) { return strcmp(*(char *const *)a, *(char *const *)b); }

char *validacao_impressao_digital(const char *raiz)
{
    GPtrArray *linhas = g_ptr_array_new_with_free_func(g_free);
    guint n = 0;
    juntar_entradas(raiz, raiz, linhas, &n);
    g_ptr_array_sort(linhas, cmp_txt);
    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA256);
    for (guint i = 0; i < linhas->len; i++) { const char *l = g_ptr_array_index(linhas, i); g_checksum_update(ck, (const guchar *)l, -1); g_checksum_update(ck, (const guchar *)"\n", 1); }
    char *r = g_strdup_printf("%s (%u entradas%s)", g_checksum_get_string(ck), n, n >= 100000 ? ", limite atingido" : "");
    g_checksum_free(ck);
    g_ptr_array_unref(linhas);
    return r;
}

static char *argv_texto(char **argv)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_array(b);
    for (int i = 0; argv[i]; i++) json_builder_add_string_value(b, argv[i]);
    json_builder_end_array(b);
    JsonGenerator *g = json_generator_new();
    JsonNode *r = json_builder_get_root(b);
    json_generator_set_root(g, r);
    char *s = json_generator_to_data(g, NULL);
    json_node_unref(r);
    g_object_unref(g);
    g_object_unref(b);
    return s;
}

char *validacao_listar(char **erro)
{
    const char *origem = NULL;
    GPtrArray *cmds = carregar(&origem, NULL, erro);
    if (!cmds) return NULL;
    char *det = NULL;
    gboolean iso = proc_isolamento_disponivel(&det);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "isolamento");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "disponivel"); json_builder_add_boolean_value(b, iso);
    json_builder_set_member_name(b, "detalhe"); json_builder_add_string_value(b, det);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "origem"); json_builder_add_string_value(b, origem);
    json_builder_set_member_name(b, "comandos");
    json_builder_begin_array(b);
    for (guint i = 0; i < cmds->len; i++) {
        Comando *c = g_ptr_array_index(cmds, i);
        char *motivo = NULL, *e2 = NULL;
        gboolean perm = validacao_argv_permitido(c->argv, &motivo);
        char *exe = perm ? proc_resolver_executavel(c->argv[0], proj_raiz(), &e2) : NULL;
        if (perm && !exe) { perm = FALSE; motivo = e2; e2 = NULL; }
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "nome"); json_builder_add_string_value(b, c->nome);
        json_builder_set_member_name(b, "argv"); json_builder_begin_array(b);
        for (int k = 0; c->argv[k]; k++) json_builder_add_string_value(b, c->argv[k]);
        json_builder_end_array(b);
        json_builder_set_member_name(b, "executavel"); if (exe) json_builder_add_string_value(b, exe); else json_builder_add_null_value(b);
        json_builder_set_member_name(b, "permitido"); json_builder_add_boolean_value(b, perm);
        json_builder_set_member_name(b, "motivo"); if (motivo) json_builder_add_string_value(b, motivo); else json_builder_add_null_value(b);
        json_builder_set_member_name(b, "timeout_segundos"); json_builder_add_int_value(b, c->timeout_s);
        json_builder_end_object(b);
        g_free(motivo); g_free(e2); g_free(exe);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    g_free(det);
    g_ptr_array_unref(cmds);
    JsonGenerator *g = json_generator_new();
    JsonNode *r = json_builder_get_root(b);
    json_generator_set_root(g, r);
    char *s = json_generator_to_data(g, NULL);
    json_node_unref(r);
    g_object_unref(g);
    g_object_unref(b);
    return s;
}

/* resolve o comando pedido: Comando (do array), executável absoluto */
static Comando *preparar(JsonObject *params, GPtrArray **cmds_out, char **exe_out, const char **origem, char **sha_cfg, char **erro)
{
    const char *nome = params && json_object_has_member(params, "comando") ? json_object_get_string_member(params, "comando") : NULL;
    GPtrArray *cmds = carregar(origem, sha_cfg, erro);
    if (!cmds) return NULL;
    char *padrao = NULL;
    if (!nome || !*nome) nome = padrao = validacao_comando_padrao();
    Comando *c = nome ? achar(cmds, nome) : NULL;
    if (!c) {
        GString *disp = g_string_new(NULL);
        for (guint i = 0; i < cmds->len; i++) g_string_append_printf(disp, "%s%s", i ? ", " : "", ((Comando *)g_ptr_array_index(cmds, i))->nome);
        *erro = g_strdup_printf("comando de validação %s%s%s não configurado (disponíveis: %s; origem: %s)", nome ? "'" : "", nome ? nome : "padrão",
                                nome ? "'" : "", disp->len ? disp->str : "nenhum", *origem);
        g_string_free(disp, TRUE);
        g_free(padrao);
        g_ptr_array_unref(cmds);
        return NULL;
    }
    g_free(padrao);
    char *motivo = NULL;
    if (!validacao_argv_permitido(c->argv, &motivo)) {
        *erro = g_strdup_printf("comando '%s' recusado: %s", c->nome, motivo);
        g_free(motivo);
        g_ptr_array_unref(cmds);
        return NULL;
    }
    char *det = NULL;
    if (!proc_isolamento_disponivel(&det)) {
        *erro = g_strdup_printf("não executo comandos do projeto sem isolamento: %s", det);
        g_free(det);
        g_ptr_array_unref(cmds);
        return NULL;
    }
    g_free(det);
    char *exe = proc_resolver_executavel(c->argv[0], proj_raiz(), erro);
    if (!exe) { g_ptr_array_unref(cmds); return NULL; }
    *cmds_out = cmds;
    *exe_out = exe;
    return c;
}

char *validacao_previa(JsonObject *params, char **erro)
{
    GPtrArray *cmds = NULL;
    char *exe = NULL, *sha_cfg = NULL;
    const char *origem = NULL;
    Comando *c = preparar(params, &cmds, &exe, &origem, &sha_cfg, erro);
    if (!c) { g_free(sha_cfg); return NULL; }
    char *av = argv_texto(c->argv);
    char *amb = proc_descricao_ambiente(NULL);
    char *digital = validacao_impressao_digital(proj_raiz());
    char *base = g_path_get_basename(exe);
    char *s = g_strdup_printf(
        "executar a validação \"%s\" do projeto\n"
        "  executável: %s\n"
        "  argv: %s\n"
        "  diretório: %s\n"
        "  ambiente: %s\n"
        "  isolamento: %s\n"
        "  limites: tempo %d s, memória %d MiB, saída %d KiB por fluxo\n"
        "  ATENÇÃO: %s executa código do próprio projeto (Makefile, scripts, testes); o isolamento limita o alcance, a sua aprovação autoriza\n"
        "  origem do comando: %s\n"
        "  configuração: %.16s · projeto: %.16s\n",
        c->nome, exe, av, proj_raiz(), amb, proc_descricao_isolamento(TRUE, FALSE), c->timeout_s, PROC_MEMORIA_PADRAO_MB,
        (int)(PROC_SAIDA_PADRAO / 1024), base, origem, sha_cfg, digital);
    g_free(base); g_free(av); g_free(amb); g_free(digital); g_free(exe); g_free(sha_cfg);
    g_ptr_array_unref(cmds);
    return s;
}

static char *ultimas_linhas(const char *t, int n, gsize max)
{
    if (!t || !*t) return g_strdup("");
    gsize len = strlen(t);
    const char *p = t + len;
    int k = 0;
    while (p > t && k <= n) { p--; if (*p == '\n' && p != t + len - 1) k++; }
    if (*p == '\n') p++;
    if (strlen(p) > max) p = t + len - max;
    while (p > t && ((unsigned char)*p & 0xC0) == 0x80) p++;
    return g_strdup(p);
}

char *validacao_executar(JsonObject *params, volatile gint *cancelado, char **erro)
{
    GPtrArray *cmds = NULL;
    char *exe = NULL;
    const char *origem = NULL;
    Comando *c = preparar(params, &cmds, &exe, &origem, NULL, erro);
    if (!c) return NULL;
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, exe);
    for (int i = 1; c->argv[i]; i++) g_ptr_array_add(a, c->argv[i]);
    g_ptr_array_add(a, NULL);
    ProcPedido p = { .argv = (char **)a->pdata, .raiz = proj_raiz(), .projeto_gravavel = TRUE, .timeout_ms = c->timeout_s * 1000,
                     .max_saida = PROC_SAIDA_PADRAO, .memoria_mb = PROC_MEMORIA_PADRAO_MB, .cancelado = cancelado };
    ProcResultado r;
    proc_executar(&p, &r);
    if (!r.executou) {
        *erro = g_strdup(r.erro ? r.erro : "não executou");
        proc_resultado_liberar(&r);
        g_ptr_array_free(a, TRUE);
        g_ptr_array_unref(cmds);
        return NULL;
    }
    gboolean passou = !r.timeout && !r.cancelado && r.sinal == 0 && r.codigo_saida == 0;
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "comando"); json_builder_add_string_value(b, c->nome);
    json_builder_set_member_name(b, "argv"); json_builder_begin_array(b);
    for (int i = 0; c->argv[i]; i++) json_builder_add_string_value(b, c->argv[i]);
    json_builder_end_array(b);
    json_builder_set_member_name(b, "executavel"); json_builder_add_string_value(b, exe);
    json_builder_set_member_name(b, "passou"); json_builder_add_boolean_value(b, passou);
    json_builder_set_member_name(b, "codigo_saida"); json_builder_add_int_value(b, r.codigo_saida);
    json_builder_set_member_name(b, "sinal"); json_builder_add_int_value(b, r.sinal);
    json_builder_set_member_name(b, "timeout"); json_builder_add_boolean_value(b, r.timeout);
    json_builder_set_member_name(b, "cancelado"); json_builder_add_boolean_value(b, r.cancelado);
    json_builder_set_member_name(b, "tempo_ms"); json_builder_add_int_value(b, r.tempo_ms);
    json_builder_set_member_name(b, "stdout"); json_builder_add_string_value(b, r.saida ? r.saida : "");
    json_builder_set_member_name(b, "stderr"); json_builder_add_string_value(b, r.erros ? r.erros : "");
    json_builder_set_member_name(b, "saida_truncada"); json_builder_add_boolean_value(b, r.stdout_truncado);
    json_builder_set_member_name(b, "erros_truncados"); json_builder_add_boolean_value(b, r.stderr_truncado);
    char *fim_o = ultimas_linhas(r.saida, 15, 2000), *fim_e = ultimas_linhas(r.erros, 15, 2000);
    json_builder_set_member_name(b, "final_saida"); json_builder_add_string_value(b, fim_o);
    json_builder_set_member_name(b, "final_erros"); json_builder_add_string_value(b, fim_e);
    char *sha_saida = g_compute_checksum_for_string(G_CHECKSUM_SHA256, r.saida ? r.saida : "", -1);
    char *sha_erros = g_compute_checksum_for_string(G_CHECKSUM_SHA256, r.erros ? r.erros : "", -1);
    char *junto = g_strconcat(sha_saida, sha_erros, NULL);
    char *sha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, junto, -1);
    json_builder_set_member_name(b, "sha256_saida"); json_builder_add_string_value(b, sha);
    json_builder_set_member_name(b, "isolamento"); json_builder_add_string_value(b, proc_descricao_isolamento(TRUE, FALSE));
    json_builder_end_object(b);
    g_free(fim_o); g_free(fim_e); g_free(sha_saida); g_free(sha_erros); g_free(junto); g_free(sha);
    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    char *s = json_generator_to_data(g, NULL);
    json_node_unref(root);
    g_object_unref(g);
    g_object_unref(b);
    proc_resultado_liberar(&r);
    g_ptr_array_free(a, TRUE);
    g_free(exe);
    g_ptr_array_unref(cmds);
    return s;
}
