/* Testes de projeto/validacao.c e projeto/git.c num projeto C temporário
 * (repositório Git próprio, sob /tmp). Sem rede, sem modelo.
 * Uso: make teste-validacao-git */
#define _GNU_SOURCE
#include "../../projeto/arquivos.h"
#include "../../projeto/git.h"
#include "../../projeto/processo.h"
#include "../../projeto/validacao.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total, falhas;
#define checar(c, d) checar_((c) ? TRUE : FALSE, (d))
static void checar_(gboolean cond, const char *desc)
{
    total++;
    if (!cond) falhas++;
    printf("%s %s\n", cond ? "ok  " : "FALHA", desc);
}

static char *raiz;

static void gravar(const char *rel, const char *conteudo)
{
    char *c = g_build_filename(raiz, rel, NULL);
    char *pai = g_path_get_dirname(c);
    g_mkdir_with_parents(pai, 0755);
    g_file_set_contents(c, conteudo, -1, NULL);
    g_free(pai);
    g_free(c);
}

static gboolean existe(const char *rel) { char *c = g_build_filename(raiz, rel, NULL); gboolean e = g_file_test(c, G_FILE_TEST_EXISTS); g_free(c); return e; }

/* git direto (fora da Lia) só para montar o cenário de teste */
static char *git_direto(const char *args)
{
    char *cmd = g_strdup_printf("git -C '%s' -c user.name=Teste -c user.email=t@t %s", raiz, args);
    char *out = NULL;
    g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL);
    g_free(cmd);
    return out ? out : g_strdup("");
}

static JsonObject *params(const char *json)
{
    JsonParser *p = json_parser_new();
    json_parser_load_from_data(p, json, -1, NULL);
    JsonObject *o = json_object_ref(json_node_get_object(json_parser_get_root(p)));
    g_object_unref(p);
    return o;
}

static char *validar(const char *json, volatile gint *canc, char **erro)
{
    JsonObject *o = params(json);
    *erro = NULL;
    char *r = validacao_executar(o, canc, erro);
    json_object_unref(o);
    return r;
}

static char *previa(const char *json, char **erro)
{
    JsonObject *o = params(json);
    *erro = NULL;
    char *r = validacao_previa(o, erro);
    json_object_unref(o);
    return r;
}

static gboolean tem(const char *s, const char *t) { return s && strstr(s, t); }

typedef struct { volatile gint *f; } Canc;
static gpointer cancelar(gpointer d) { g_usleep(600000); g_atomic_int_set(((Canc *)d)->f, 1); return NULL; }

static const char *MAKEFILE =
    "CFLAGS=-Wall -Wextra -Werror\n"
    "teste: teste_soma\n\t./teste_soma\n"
    "teste_soma: teste_soma.c soma.c\n\t$(CC) $(CFLAGS) -o $@ teste_soma.c soma.c\n"
    "lento:\n\tsleep 20\n"
    "barulho:\n\tyes linha-de-saida | head -c 3000000\n"
    "rede:\n\tcat /proc/net/dev\n";

int main(void)
{
    raiz = g_dir_make_tmp("lia-valgit-XXXXXX", NULL);
    char *desf = g_strdup_printf("%s-desfazer", raiz);
    proj_definir_dir_desfazer(desf);
    char *erro = NULL;
    if (!proj_definir_raiz(raiz, &erro)) { printf("raiz: %s\n", erro); return 2; }
    char *det = NULL;
    if (!proc_isolamento_disponivel(&det)) {
        printf("isolamento indisponível (%s): conferindo só a falha fechada\n", det);
        gravar("Makefile", MAKEFILE);
        char *r = previa("{\"comando\":\"teste\"}", &erro);
        checar(!r && tem(erro, "sem isolamento"), "sem isolamento: validação recusada antes de pedir aprovação");
        printf("\n%d testes, %d falhas\n", total, falhas);
        return falhas ? 1 : 0;
    }
    g_free(det);

    gravar("soma.c", "int soma(int a, int b) { return a - b; }\n");
    gravar("teste_soma.c", "#include <stdio.h>\nint soma(int, int);\nint main(void) { if (soma(2, 3) != 5) { printf(\"FALHA soma(2,3)=%d\\n\", soma(2, 3)); return 1; } puts(\"ok\"); return 0; }\n");
    gravar("Makefile", MAKEFILE);

    /* ---------------- validação ---------------- */
    char *l = validacao_listar(&erro);
    checar(tem(l, "\"nome\":\"build\"") && tem(l, "\"nome\":\"teste\"") && tem(l, "detectado no Makefile") && tem(l, "\"disponivel\":true"),
           "listar: comandos detectados no Makefile, isolamento disponível");
    g_free(l);
    char *pad = validacao_comando_padrao();
    checar(!g_strcmp0(pad, "teste"), "comando padrão para 'rode os testes' = teste");
    g_free(pad);

    char *pv = previa("{\"comando\":\"teste\"}", &erro);
    checar(tem(pv, "executável: /usr/bin/make") && tem(pv, "argv: [\"make\",\"teste\"]") && tem(pv, raiz) && tem(pv, "nada herdado") &&
           tem(pv, "sem rede") && tem(pv, "executa código do próprio projeto") && tem(pv, "tempo 120 s"),
           "prévia: executável, argv, diretório, ambiente, isolamento, limites e o aviso de que roda código do projeto");
    char *pv2 = previa("{\"comando\":\"teste\"}", &erro);
    checar(pv && pv2 && !strcmp(pv, pv2), "prévia estável enquanto nada muda");
    g_free(pv2);
    gravar("novo-arquivo.txt", "x\n");
    pv2 = previa("{\"comando\":\"teste\"}", &erro);
    checar(pv2 && strcmp(pv, pv2), "prévia muda quando o projeto muda (a aprovação anterior deixa de valer)");
    g_free(pv2); g_free(pv);

    char *r = validar("{\"comando\":\"teste\"}", NULL, &erro);
    checar(tem(r, "\"passou\":false") && tem(r, "\"codigo_saida\":2") && tem(r, "FALHA soma(2,3)=-1"),
           "teste que falha: passou=false, código do make e a mensagem do teste preservados");
    g_free(r);
    gravar("soma.c", "int soma(int a, int b) { return a + b; }\n");
    r = validar("{\"comando\":\"teste\"}", NULL, &erro);
    checar(tem(r, "\"passou\":true") && tem(r, "\"codigo_saida\":0") && tem(r, "\"final_saida\":\"") && existe("teste_soma"),
           "depois da correção: compila dentro do isolamento e passa");
    g_free(r);
    r = validar("{\"comando\":\"inexistente\"}", NULL, &erro);
    checar(!r && tem(erro, "não configurado") && tem(erro, "teste"), "comando não configurado: erro com os disponíveis");
    g_clear_pointer(&erro, g_free);

    gravar(".lia/validacao.json",
           "{\"comandos\":{\"teste\":[\"make\",\"teste\"],\"shell\":[\"sh\",\"-c\",\"make teste && rm -rf ~\"],\"py\":[\"python3\",\"-c\",\"print(1)\"],"
           "\"rm\":[\"rm\",\"-rf\",\"soma.c\"],\"literal\":[\"make\",\"alvo; rm -rf soma.c\"],\"env\":[\"env\",\"make\"],"
           "\"lento\":{\"argv\":[\"make\",\"lento\"],\"timeout_segundos\":1},\"barulho\":[\"make\",\"barulho\"],\"rede\":[\"make\",\"rede\"]}}");
    l = validacao_listar(&erro);
    checar(tem(l, ".lia/validacao.json") && tem(l, "não confiável"), "listar: origem = configuração do projeto, marcada como não confiável");
    g_free(l);
    pv = previa("{\"comando\":\"shell\"}", &erro);
    checar(!pv && tem(erro, "shell"), "configuração do projeto não consegue rodar um shell");
    g_clear_pointer(&erro, g_free);
    pv = previa("{\"comando\":\"env\"}", &erro);
    checar(!pv && tem(erro, "executa outros comandos"), "nem um 'embrulho' como env");
    g_clear_pointer(&erro, g_free);
    pv = previa("{\"comando\":\"py\"}", &erro);
    checar(!pv && tem(erro, "código em linha"), "nem código em linha (python3 -c)");
    g_clear_pointer(&erro, g_free);
    pv = previa("{\"comando\":\"rm\"}", &erro);
    checar(!pv && tem(erro, "lista de executáveis permitidos"), "nem executável fora da lista permitida (rm)");
    g_clear_pointer(&erro, g_free);
    r = validar("{\"comando\":\"literal\"}", NULL, &erro);
    checar(tem(r, "alvo; rm -rf soma.c") && tem(r, "\"passou\":false") && existe("soma.c"),
           "argv literal: 'alvo; rm -rf soma.c' chega ao make como um nome de alvo, nada é apagado");
    g_free(r);
    r = validar("{\"comando\":\"lento\"}", NULL, &erro);
    checar(tem(r, "\"timeout\":true") && tem(r, "\"passou\":false"), "timeout da configuração respeitado (1 s)");
    g_free(r);
    r = validar("{\"comando\":\"barulho\"}", NULL, &erro);
    checar(tem(r, "\"saida_truncada\":true") && strlen(r) < 700 * 1024, "saída excessiva cortada e marcada");
    g_free(r);
    r = validar("{\"comando\":\"rede\"}", NULL, &erro);
    checar(tem(r, "lo:") && !tem(r, "eth0") && !tem(r, "wlan"), "build sem rede (só lo)");
    g_free(r);
    volatile gint flag = 0;
    Canc cc = { &flag };
    GThread *th = g_thread_new("c", cancelar, &cc);
    gint64 t0 = g_get_monotonic_time();
    gravar(".lia/validacao.json", "{\"comandos\":{\"lento\":[\"make\",\"lento\"]}}");
    r = validar("{\"comando\":\"lento\"}", &flag, &erro);
    g_thread_join(th);
    checar(tem(r, "\"cancelado\":true") && (g_get_monotonic_time() - t0) / 1000 < 5000, "cancelamento encerra a validação em segundos");
    g_free(r);
    pv = previa("{\"comando\":\"lento\"}", &erro);
    gravar(".lia/validacao.json", "{\"comandos\":{\"lento\":[\"make\",\"lento\",\"-j2\"]}}");
    pv2 = previa("{\"comando\":\"lento\"}", &erro);
    checar(pv && pv2 && strcmp(pv, pv2), "prévia muda quando a configuração muda");
    g_free(pv); g_free(pv2);
    gravar(".lia/validacao.json", "isto não é json");
    pv = previa("{\"comando\":\"teste\"}", &erro);
    checar(!pv && tem(erro, "inválido"), "configuração inválida: recusada com motivo");
    g_clear_pointer(&erro, g_free);
    char *cfg = g_build_filename(raiz, ".lia", "validacao.json", NULL);
    g_unlink(cfg);
    char *alvo_link = g_build_filename(raiz, "Makefile", NULL);
    if (symlink(alvo_link, cfg) == 0) {
        pv = previa("{\"comando\":\"teste\"}", &erro);
        checar(!pv && tem(erro, "link simbólico"), "configuração como link simbólico: recusada");
        g_clear_pointer(&erro, g_free);
        g_unlink(cfg);
    }
    g_free(cfg); g_free(alvo_link);

    /* ---------------- Git ---------------- */
    char *o = git_status(NULL, &erro);
    checar(!o && tem(erro, "não é a raiz de um repositório"), "git: pasta sem .git/ -> erro claro");
    g_clear_pointer(&erro, g_free);
    g_free(git_direto("init -q -b principal"));
    g_free(git_direto("add Makefile soma.c teste_soma.c"));
    g_free(git_direto("commit -q -m inicial"));
    gravar(".gitignore", "teste_soma\n");
    o = git_status(NULL, &erro);
    checar(tem(o, "\"ramo\":\"principal\"") && tem(o, "\"caminho\":\".gitignore\"") && tem(o, "\"estado\":\"??\""), "git.status: ramo e arquivos não rastreados");
    g_free(o);
    gravar("soma.c", "int soma(int a, int b) { return b + a; }\n");
    JsonObject *pd = params("{}");
    o = git_diff(pd, NULL, &erro);
    checar(tem(o, "-int soma(int a, int b) { return a + b; }") && tem(o, "+int soma(int a, int b) { return b + a; }"), "git.diff: alteração com - e +");
    g_free(o);
    json_object_unref(pd);
    pd = params("{\"limite\":5}");
    o = git_log(pd, NULL, &erro);
    checar(tem(o, "\"assunto\":\"inicial\"") && tem(o, "\"autor\":\"Teste\""), "git.log: commit, autor e assunto");
    g_free(o);
    json_object_unref(pd);

    /* ganchos plantados no repositório nunca rodam */
    gravar(".git/hooks/pre-commit", "#!/bin/sh\ntouch GANCHO-RODOU\n");
    gravar(".git/hooks/post-commit", "#!/bin/sh\ntouch GANCHO-RODOU\n");
    char *h1 = g_build_filename(raiz, ".git/hooks/pre-commit", NULL), *h2 = g_build_filename(raiz, ".git/hooks/post-commit", NULL);
    g_chmod(h1, 0755); g_chmod(h2, 0755);
    g_free(h1); g_free(h2);
    /* algo preparado pela pessoa fora do commit continua preparado */
    gravar("outro.c", "int outro;\n");
    g_free(git_direto("add outro.c"));

    git_definir_autor("Lia Teste <lia@teste.local>");
    pd = params("{\"mensagem\":\"Corrige a soma\",\"arquivos\":[\"soma.c\",\".gitignore\"]}");
    char *cp = git_commit_previa(pd, &erro);
    checar(tem(cp, "commit no ramo principal") && tem(cp, "│ Corrige a soma") && tem(cp, "arquivos incluídos (2)") && tem(cp, " M soma.c") &&
           tem(cp, "?? .gitignore") && tem(cp, "+int soma(int a, int b) { return b + a; }") && tem(cp, "+teste_soma") && tem(cp, "ganchos do Git desativados"),
           "git.commit prévia: ramo, mensagem, arquivos exatos e o diff de cada um");
    char *cp2 = git_commit_previa(pd, &erro);
    checar(cp && cp2 && !strcmp(cp, cp2), "prévia do commit estável enquanto nada muda");
    g_free(cp2);
    gravar("soma.c", "int soma(int a, int b) { return a + b + 0; }\n");
    cp2 = git_commit_previa(pd, &erro);
    checar(cp2 && strcmp(cp, cp2), "prévia do commit muda quando o diff muda (aprovação deixa de valer)");
    g_free(cp2); g_free(cp);
    char *c = git_commit_aplicar(pd, NULL, &erro);
    checar(c && tem(c, "\"commit\":\"") && tem(c, "\"soma.c\""), "git.commit: commit criado");
    if (!c) printf("      %s\n", erro);
    g_free(c);
    json_object_unref(pd);
    checar(!existe("GANCHO-RODOU"), "ganchos pre-commit/post-commit do repositório NÃO rodaram");
    char *st = git_direto("status --porcelain");
    checar(tem(st, "A  outro.c") && !tem(st, "soma.c") && !tem(st, ".gitignore"), "o que estava preparado fora do commit continua preparado");
    g_free(st);
    char *lg = git_direto("log -1 --format=%an|%s");
    checar(tem(lg, "Lia Teste|Corrige a soma"), "autor e mensagem do commit corretos");
    g_free(lg);

    pd = params("{\"mensagem\":\"nada\",\"arquivos\":[\"soma.c\"]}");
    cp = git_commit_previa(pd, &erro);
    checar(!cp && tem(erro, "não tem alterações"), "commit de arquivo sem alteração: recusado");
    g_clear_pointer(&erro, g_free);
    json_object_unref(pd);
    pd = params("{\"mensagem\":\"x\",\"arquivos\":[\"../fora.c\"]}");
    cp = git_commit_previa(pd, &erro);
    checar(!cp && tem(erro, "fora do projeto"), "commit com arquivo fora do projeto: recusado");
    g_clear_pointer(&erro, g_free);
    json_object_unref(pd);
    pd = params("{\"mensagem\":\"   \"}");
    cp = git_commit_previa(pd, &erro);
    checar(!cp && tem(erro, "mensagem"), "commit sem mensagem: recusado");
    g_clear_pointer(&erro, g_free);
    json_object_unref(pd);
    pd = params("{\"mensagem\":\"com \\u001b[31m controle\"}");
    cp = git_commit_previa(pd, &erro);
    checar(!cp && tem(erro, "controle"), "mensagem com caractere de controle: recusada");
    g_clear_pointer(&erro, g_free);
    json_object_unref(pd);
    pd = params("{\"mensagem\":\"tudo\"}");
    cp = git_commit_previa(pd, &erro);
    checar(tem(cp, "arquivos incluídos (2)") && tem(cp, "A  outro.c") && tem(cp, "?? novo-arquivo.txt"), "sem lista: TODOS os alterados, mostrados um a um na prévia");
    g_free(cp);
    gravar(".git/MERGE_HEAD", "0000000000000000000000000000000000000000\n");
    cp = git_commit_previa(pd, &erro);
    checar(!cp && tem(erro, "em andamento"), "merge em andamento: commit recusado");
    g_clear_pointer(&erro, g_free);
    json_object_unref(pd);
    char *mh = g_build_filename(raiz, ".git/MERGE_HEAD", NULL);
    g_unlink(mh);
    g_free(mh);

    /* ---------------- repositório com pack maior que o limite de memória ---------------- */
    {
        char *grande = g_strdup_printf("%s-grande", raiz);
        g_mkdir_with_parents(grande, 0755);
        char *cmd_g = g_strdup_printf("cd '%s' && git init -q -b principal && head -c 180000000 /dev/urandom > dados.bin && "
                                      "git -c user.name=T -c user.email=t@t add dados.bin && git -c user.name=T -c user.email=t@t commit -q -m grande && "
                                      "git -c gc.auto=0 repack -q -a -d", grande);
        int rc = system(cmd_g);
        g_free(cmd_g);
        char *e2 = NULL;
        proj_definir_raiz(grande, &e2);
        g_setenv("LIA_SESSIOND_GIT_MEMORIA_MB", "128", TRUE);
        checar(rc == 0 && git_memoria_mb() == 128, "pack grande: repositório de ~180 MB criado; git isolado limitado a 128 MiB");
        /* controle negativo: o git SEM janelas de mmap limitadas falha como no POC */
        char *gitbin = g_strdup("/usr/bin/git");
        char *argv_neg[] = { gitbin, (char *)"status", (char *)"--porcelain", NULL };
        char *env_neg[] = { (char *)"GIT_CONFIG_NOSYSTEM=1", (char *)"GIT_CONFIG_GLOBAL=/dev/null", NULL };
        ProcPedido pn = { .argv = argv_neg, .raiz = grande, .env_extra = env_neg, .timeout_ms = 60000, .max_saida = 65536, .memoria_mb = 128 };
        ProcResultado rn;
        proc_executar(&pn, &rn);
        checar(rn.executou && rn.codigo_saida != 0 && rn.erros && strstr(rn.erros, "cannot be mapped"),
               "controle: sem janelas limitadas, o git isolado falha ('packfile cannot be mapped'), como no POC");
        proc_resultado_liberar(&rn);
        g_free(gitbin);
        char *st = git_status(NULL, &e2);
        checar(st && tem(st, "\"limpo\":true"), "git.status da Lia funciona no mesmo repositório e limite (janelas de mmap limitadas)");
        if (!st) printf("      %s\n", e2);
        g_free(st);
        g_clear_pointer(&e2, g_free);
        JsonObject *pl = params("{\"limite\":1}");
        st = git_log(pl, NULL, &e2);
        checar(st && tem(st, "\"assunto\":\"grande\""), "git.log também funciona no repositório grande");
        g_free(st);
        json_object_unref(pl);
        g_unsetenv("LIA_SESSIOND_GIT_MEMORIA_MB");
        g_setenv("LIA_SESSIOND_GIT_MEMORIA_MB", "999999", TRUE);
        checar(git_memoria_mb() == 4096, "limite configurável com teto (999999 -> 4096 MiB)");
        g_setenv("LIA_SESSIOND_GIT_MEMORIA_MB", "1", TRUE);
        checar(git_memoria_mb() == 128, "limite configurável com piso (1 -> 128 MiB)");
        g_unsetenv("LIA_SESSIOND_GIT_MEMORIA_MB");
        char *rm = g_strdup_printf("rm -rf -- '%s'", grande);
        if (g_str_has_prefix(grande, "/tmp/lia-valgit-")) { int r2 = system(rm); (void)r2; }
        g_free(rm);
        g_free(grande);
    }

    printf("\n%d testes, %d falhas\n", total, falhas);
    char *cmd = g_strdup_printf("rm -rf -- '%s' '%s'", raiz, desf);
    if (g_str_has_prefix(raiz, "/tmp/lia-valgit-")) { int rc = system(cmd); (void)rc; }
    g_free(cmd); g_free(desf);
    return falhas ? 1 : 0;
}
