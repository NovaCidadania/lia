/* Testes de projeto/processo.c: isolamento real (bubblewrap), argv
 * literal, ambiente limpo, limites, timeout, cancelamento, saída
 * excessiva. Uso: make teste-processo (precisa de bwrap funcional; sem
 * ele, confere que nada roda — falha fechada). */
#define _GNU_SOURCE
#include "../../projeto/processo.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int total, falhas;
#define checar(c, d) checar_((c) ? TRUE : FALSE, (d))
static void checar_(gboolean cond, const char *desc)
{
    total++;
    if (!cond) falhas++;
    printf("%s %s\n", cond ? "ok  " : "FALHA", desc);
}

static char *raiz;

static void rodar(char **argv_nomes, ProcPedido *base, ProcResultado *r)
{
    char *erro = NULL;
    char *exe = proc_resolver_executavel(argv_nomes[0], raiz, &erro);
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, exe);
    for (int i = 1; argv_nomes[i]; i++) g_ptr_array_add(a, argv_nomes[i]);
    g_ptr_array_add(a, NULL);
    ProcPedido p = *base;
    p.argv = (char **)a->pdata;
    p.raiz = raiz;
    proc_executar(&p, r);
    g_free(exe);
    g_free(erro);
    g_ptr_array_free(a, TRUE);
}

#define ARGV(...) ((char *[]){ __VA_ARGS__, NULL })

typedef struct { volatile gint *flag; int atraso_ms; } Cancelador;
static gpointer cancelar_depois(gpointer d)
{
    Cancelador *c = d;
    g_usleep((gulong)c->atraso_ms * 1000);
    g_atomic_int_set(c->flag, 1);
    return NULL;
}

int main(void)
{
    raiz = g_dir_make_tmp("lia-processo-XXXXXX", NULL);
    char *git = g_build_filename(raiz, ".git", NULL);
    g_mkdir(git, 0755);
    char *head = g_build_filename(git, "HEAD", NULL);
    g_file_set_contents(head, "ref: refs/heads/master\n", -1, NULL);
    g_setenv("LIA_SEGREDO_DE_TESTE", "nao-pode-vazar", TRUE);

    char *det = NULL;
    gboolean iso = proc_isolamento_disponivel(&det);
    printf("isolamento: %s\n", det);
    g_free(det);
    if (!iso) {
        ProcPedido p = { .timeout_ms = 1000 };
        ProcResultado r;
        rodar(ARGV("true"), &p, &r);
        checar(!r.executou && r.erro && strstr(r.erro, "isolamento indisponível"), "sem bwrap: nada é executado (falha fechada)");
        proc_resultado_liberar(&r);
        printf("\n%d testes, %d falhas\n", total, falhas);
        return falhas ? 1 : 0;
    }

    char *erro = NULL;
    char *e = proc_resolver_executavel("make", raiz, &erro);
    checar(e && g_str_has_prefix(e, "/usr/"), "resolver: 'make' encontrado em /usr");
    g_free(e);
    e = proc_resolver_executavel("comando-que-nao-existe-xyz", raiz, &erro);
    checar(!e && erro && strstr(erro, "não encontrado"), "resolver: comando inexistente");
    g_clear_pointer(&erro, g_free);
    e = proc_resolver_executavel("../fora", raiz, &erro);
    checar(!e && erro && strstr(erro, "fora do projeto"), "resolver: caminho relativo fora do projeto recusado");
    g_clear_pointer(&erro, g_free);
    e = proc_resolver_executavel("/etc/passwd", raiz, &erro);
    checar(!e && erro, "resolver: absoluto fora de /usr recusado");
    g_clear_pointer(&erro, g_free);
    e = proc_resolver_executavel(".git/hooks/x", raiz, &erro);
    checar(!e && erro, "resolver: executável dentro de .git/ recusado");
    g_clear_pointer(&erro, g_free);

    ProcPedido base = { .timeout_ms = 10000, .max_saida = 64 * 1024, .memoria_mb = 1024, .projeto_gravavel = TRUE };
    ProcResultado r;

    rodar(ARGV("true"), &base, &r);
    checar(r.executou && r.codigo_saida == 0 && !r.timeout, "true: executa e sai com 0");
    proc_resultado_liberar(&r);
    rodar(ARGV("false"), &base, &r);
    checar(r.executou && r.codigo_saida == 1, "false: código de saída 1 preservado");
    proc_resultado_liberar(&r);

    rodar(ARGV("printf", "%s|%s", "a; rm -rf / && echo $HOME `id`", "*"), &base, &r);
    checar(r.saida && !strcmp(r.saida, "a; rm -rf / && echo $HOME `id`|*"), "argv literal: metacaracteres de shell chegam como texto, sem shell no meio");
    proc_resultado_liberar(&r);

    rodar(ARGV("env"), &base, &r);
    checar(r.saida && strstr(r.saida, "HOME=/tmp/home") && strstr(r.saida, "PATH=/usr/local/bin:/usr/bin") &&
           !strstr(r.saida, "LIA_SEGREDO_DE_TESTE") && !strstr(r.saida, "DBUS") && !strstr(r.saida, "SSH_AUTH"),
           "ambiente limpo: só as variáveis fixas, nada herdado (nem segredos do daemon)");
    proc_resultado_liberar(&r);

    rodar(ARGV("pwd"), &base, &r);
    char *esperado = g_strconcat(raiz, "\n", NULL);
    checar(r.saida && !strcmp(r.saida, esperado), "diretório de trabalho = raiz do projeto");
    g_free(esperado);
    proc_resultado_liberar(&r);

    rodar(ARGV("ls", "/home"), &base, &r);
    checar(r.codigo_saida != 0, "isolamento: /home não existe dentro do processo");
    proc_resultado_liberar(&r);
    rodar(ARGV("cat", "/proc/net/dev"), &base, &r);
    checar(r.codigo_saida == 0 && r.saida && strstr(r.saida, "lo:") && !strstr(r.saida, "eth") && !strstr(r.saida, "wl") && !strstr(r.saida, "en"),
           "isolamento: sem rede (só a interface lo)");
    proc_resultado_liberar(&r);
    rodar(ARGV("touch", "criado-no-teste.txt"), &base, &r);
    char *criado = g_build_filename(raiz, "criado-no-teste.txt", NULL);
    checar(r.codigo_saida == 0 && g_file_test(criado, G_FILE_TEST_EXISTS), "projeto gravável quando pedido");
    g_free(criado);
    proc_resultado_liberar(&r);
    rodar(ARGV("touch", ".git/hooks-novo"), &base, &r);
    checar(r.codigo_saida != 0, ".git/ somente leitura (um build não planta ganchos do Git)");
    proc_resultado_liberar(&r);
    ProcPedido ro = base;
    ro.projeto_gravavel = FALSE;
    rodar(ARGV("touch", "nao-pode.txt"), &ro, &r);
    checar(r.codigo_saida != 0, "projeto somente leitura quando pedido (consultas Git)");
    proc_resultado_liberar(&r);
    rodar(ARGV("touch", "/usr/lia-teste"), &base, &r);
    checar(r.codigo_saida != 0, "sistema somente leitura");
    proc_resultado_liberar(&r);

    ProcPedido curto = base;
    curto.timeout_ms = 500;
    gint64 t0 = g_get_monotonic_time();
    rodar(ARGV("sleep", "30"), &curto, &r);
    checar(r.timeout && r.codigo_saida != 0 && (g_get_monotonic_time() - t0) / 1000 < 4000, "timeout: encerra em segundos, marcado como timeout");
    proc_resultado_liberar(&r);

    ProcPedido pouca = base;
    pouca.max_saida = 1000;
    pouca.timeout_ms = 1500;
    rodar(ARGV("yes", "linha"), &pouca, &r);
    checar(r.stdout_truncado && strlen(r.saida) <= 1000, "saída excessiva: cortada no limite e marcada (o processo nunca trava no pipe)");
    proc_resultado_liberar(&r);

    volatile gint flag = 0;
    Cancelador cc = { &flag, 400 };
    GThread *th = g_thread_new("cancelador", cancelar_depois, &cc);
    ProcPedido canc = base;
    canc.cancelado = &flag;
    t0 = g_get_monotonic_time();
    /* processo com filhos: o cancelamento derruba a árvore inteira */
    rodar(ARGV("sh", "-c", "sleep 101.25 & sleep 101.25 & wait"), &canc, &r);
    g_thread_join(th);
    checar(r.cancelado && (g_get_monotonic_time() - t0) / 1000 < 4000, "cancelamento: encerra rápido e marca cancelado");
    proc_resultado_liberar(&r);
    g_usleep(300000);
    gint st = 0;
    char *saida_pg = NULL;
    g_spawn_command_line_sync("pgrep -f 'sleep 101.25'", &saida_pg, NULL, &st, NULL);
    checar(!saida_pg || !*saida_pg, "nenhum processo filho sobrevive ao cancelamento");
    g_free(saida_pg);

    ProcPedido mem = base;
    mem.memoria_mb = 64;
    rodar(ARGV("python3", "-c", "x = bytearray(512 * 1024 * 1024)"), &mem, &r);
    checar(r.executou && r.codigo_saida != 0, "limite de memória aplicado ao processo");
    proc_resultado_liberar(&r);

    char **av = proc_argv_isolado(&(ProcPedido){ .argv = ARGV("/usr/bin/true"), .raiz = raiz });
    char *junto = g_strjoinv(" ", av);
    checar(strstr(junto, "--unshare-all") && strstr(junto, "--clearenv") && strstr(junto, "--die-with-parent") && strstr(junto, "--ro-bind /usr /usr"),
           "argv do bwrap: namespaces, ambiente limpo, morte junto com o pai, sistema somente leitura");
    g_free(junto);
    g_strfreev(av);

    printf("\n%d testes, %d falhas\n", total, falhas);
    char *cmd = g_strdup_printf("rm -rf -- '%s'", raiz);
    if (g_str_has_prefix(raiz, "/tmp/lia-processo-")) { int rc = system(cmd); (void)rc; }
    g_free(cmd); g_free(git); g_free(head);
    return falhas ? 1 : 0;
}
