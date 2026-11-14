/* lia-processos-teste — testes internos de lia_proc_coletar_amostra()/
 * lia_proc_calcular_cpu() (cli/sistema.c, cli/cli.h). Programa freestanding
 * próprio (sem libc), igual em espírito a testes/mem/teste_mem.c: constrói,
 * em tempo de execução, suas próprias árvores de fixtures de "/proc" sob o
 * diretório passado em argv[1] (um `mktemp -d` de verdade, nunca o /proc
 * real — isso é o que torna o cálculo testável sem depender da máquina
 * real nem de nenhum timing/sleep real). Roda via `make teste`
 * (testes/teste.sh invoca o binário e reaproveita as linhas "ok "/"FALHA "
 * dele, mesmo padrão do lia-mem-teste).
 *
 * Uso: lia-processos-teste <diretório_escrevível_vazio>
 */
#include "../../runtime/lia.h"
#include "../../cli/cli.h"

/* cli/sistema.c referencia lia_out/lia_err/lia_juntar (definidos em
 * cli/main.c/cli/ferramentas.c normalmente) — este binário de teste não
 * linka main.o (teria um segundo lia_main, conflitando com o nosso) nem
 * ferramentas.o (arrastaria lia_fork/lia_execve sem necessidade nenhuma
 * para testar só coleta/cálculo), então fornece as três coisas aqui,
 * mínimas e exatamente com a mesma assinatura/contrato do resto do
 * projeto. */
Buf lia_out, lia_err;
static char mem_out[4096], mem_err[4096];

int lia_juntar(char *dst, size_t cap, const char *a, const char *b, const char *c)
{
    size_t la = lia_strlen(a), lb = lia_strlen(b), lc = lia_strlen(c);
    if (la + lb + lc + 1 > cap)
        return -1;
    memcpy(dst, a, la);
    memcpy(dst + la, b, lb);
    memcpy(dst + la + lb, c, lc);
    dst[la + lb + lc] = 0;
    return 0;
}

static Buf g_out;
static long g_total = 0, g_falhas = 0;
static char g_raiz[256];
static char g_subraiz[256]; /* raiz de fixtures do teste ATUAL — ver nova_subraiz() */

static void checar(int cond, const char *descricao)
{
    g_total++;
    if (cond) {
        b_s(&g_out, "ok   ");
    } else {
        g_falhas++;
        b_s(&g_out, "FALHA ");
    }
    b_s(&g_out, descricao);
    b_c(&g_out, '\n');
}

static void escrever_arquivo(const char *caminho, const char *conteudo)
{
    long fd = lia_openat(AT_FDCWD, caminho, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    size_t n = lia_strlen(conteudo);
    size_t off = 0;
    while (off < n) {
        long r = lia_write((int)fd, conteudo + off, n - off);
        if (r <= 0)
            break;
        off += (size_t)r;
    }
    lia_close((int)fd);
}

/* Cada teste chama isto primeiro: cria "<g_raiz>/<nome>" vazio e passa a
 * usá-lo como raiz de fixtures (g_subraiz) — nunca o g_raiz inteiro
 * diretamente, senão PIDs de um teste contaminariam a contagem de outro
 * (todos usam os mesmos números de PID pequenos, de propósito, para
 * ficar claro de ler). */
static void nova_subraiz(const char *nome)
{
    lia_juntar(g_subraiz, sizeof g_subraiz, g_raiz, "/", nome);
    lia_mkdirat(AT_FDCWD, g_subraiz, 0700);
}

/* Monta "<g_subraiz>/<sub>/<nome>" (sub/nome podem ser "" — junta só o
 * que não for vazio) com 'lia_juntar' em duas etapas, igual ao resto do
 * projeto já faz para caminhos de /proc. */
static void caminho(char *dst, size_t cap, const char *sub, const char *nome)
{
    char tmp[256];
    lia_juntar(tmp, sizeof tmp, g_subraiz, "/", sub);
    if (nome && *nome)
        lia_juntar(dst, cap, tmp, "/", nome);
    else {
        size_t n = lia_strlen(tmp);
        if (n + 1 <= cap) { memcpy(dst, tmp, n); dst[n] = 0; }
    }
}

/* Cria "<g_subraiz>/<pid>/" e grava seu "stat" com o conteúdo dado
 * (texto já pronto, do jeito que o kernel escreveria — ou
 * deliberadamente malformado, para os casos de erro). */
static void criar_processo(const char *pid_str, const char *conteudo_stat)
{
    char dirp[256], statp[256];
    caminho(dirp, sizeof dirp, pid_str, "");
    lia_mkdirat(AT_FDCWD, dirp, 0700);
    caminho(statp, sizeof statp, pid_str, "stat");
    escrever_arquivo(statp, conteudo_stat);
}

/* Linha de /proc/<pid>/stat realista, dados os dois campos que importam
 * (utime, stime) e um comm/estado escolhidos pelo chamador. Os outros
 * dez campos entre comm e utime recebem valores fixos plausíveis
 * (alguns negativos, como tpgid de verdade costuma vir quando não há
 * terminal controlador — testa que o sinal é aceito e descartado). */
static void montar_stat(char *dst, size_t cap, const char *pid, const char *comm, char estado, u64 utime, u64 stime)
{
    char utime_s[24], stime_s[24];
    Buf b;
    b_init(&b, utime_s, sizeof utime_s, -1); b_u(&b, utime); utime_s[b.n] = 0;
    b_init(&b, stime_s, sizeof stime_s, -1); b_u(&b, stime); stime_s[b.n] = 0;

    /* ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt */
    char buf[512];
    size_t n = 0;
    #define CAT(s) do { size_t ls = lia_strlen(s); if (n + ls < sizeof buf) { memcpy(buf + n, s, ls); n += ls; } } while (0)
    CAT(pid); CAT(" ("); CAT(comm); CAT(") ");
    char estado_s[2] = { estado, 0 };
    CAT(estado_s);
    CAT(" 1 1 1 0 -1 4194304 0 0 0 0 "); /* ppid..cmajflt, tpgid=-1 de propósito */
    CAT(utime_s); CAT(" "); CAT(stime_s);
    CAT(" 20 0 20 0 1 0 123456 4096000 512 18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    #undef CAT
    buf[n] = 0;
    size_t cp = n + 1 < cap ? n + 1 : cap;
    memcpy(dst, buf, cp);
    dst[cap - 1] = 0;
}

/* ---------------------------------------------------------- testes --- */

static void teste_coletar_basico(void)
{
    nova_subraiz("basico");
    char statline[512];
    montar_stat(statline, sizeof statline, "100", "minhaferramenta", 'S', 250, 50);
    criar_processo("100", statline);
    montar_stat(statline, sizeof statline, "200", "outro", 'R', 10, 10);
    criar_processo("200", statline);
    criar_processo("abc", "isto nem deveria ser lido"); /* nome não-numérico: não é PID */
    criar_processo("123abc", "idem — mistura dígito com letra");

    LiaProcAmostra a[16];
    long n = lia_proc_coletar_amostra(g_subraiz, a, 16);
    checar(n == 2, "coletar: só os dois diretórios puramente numéricos contam como PID");

    int achou_100 = 0, achou_200 = 0;
    for (long i = 0; i < n; i++) {
        if (a[i].pid == 100) {
            achou_100 = 1;
            checar(!lia_strcmp(a[i].nome, "minhaferramenta"), "coletar: nome (comm) extraído corretamente");
            checar(a[i].estado == 'S', "coletar: estado extraído corretamente");
            checar(a[i].ticks == 300, "coletar: utime+stime somados corretamente (250+50)");
        }
        if (a[i].pid == 200)
            achou_200 = 1;
    }
    checar(achou_100, "coletar: pid 100 presente");
    checar(achou_200, "coletar: pid 200 presente");
}

static void teste_coletar_nome_com_parenteses(void)
{
    /* comm pode conter ')' — a extração deve usar a ÚLTIMA ')' da linha,
     * nunca a primeira depois da primeira '(', senão corta o nome errado.
     * Também exercita um comm com aspas/chaves (como um texto de injeção
     * de prompt tentaria usar) — nunca interpretado como instrução por
     * este código (só é texto coletado, igual a qualquer outro campo);
     * a camada que de fato precisa escapar isso para JSON é b_json(),
     * exercitada no teste de ponta a ponta via testes/teste.sh. */
    nova_subraiz("parenteses");
    char statline[512];
    /* 10 bytes — bem dentro de LIA_PROC_NOME_MAX (32), para este teste
     * verificar só a lógica de parênteses/aspas, nunca o truncamento
     * (esse é um comportamento esperado e separado, não testado aqui). */
    montar_stat(statline, sizeof statline, "300", "a(b)c\"d{e}", 'S', 1, 1);
    criar_processo("300", statline);

    LiaProcAmostra a[4];
    long n = lia_proc_coletar_amostra(g_subraiz, a, 4);
    checar(n == 1, "coletar: processo com parênteses/aspas no nome ainda é coletado");
    if (n == 1)
        checar(!lia_strcmp(a[0].nome, "a(b)c\"d{e}"),
               "coletar: nome com parênteses internos extraído até a ÚLTIMA ')'");
}

static void teste_coletar_nome_truncado_com_seguranca(void)
{
    nova_subraiz("nome_longo");
    char statline[512];
    /* 40 bytes, acima de LIA_PROC_NOME_MAX (32) — nunca deveria
     * transbordar o buffer, só truncar para os primeiros 31 + NUL. */
    montar_stat(statline, sizeof statline, "301", "0123456789012345678901234567890123456789", 'S', 1, 1);
    criar_processo("301", statline);

    LiaProcAmostra a[4];
    long n = lia_proc_coletar_amostra(g_subraiz, a, 4);
    checar(n == 1, "coletar: processo com nome maior que LIA_PROC_NOME_MAX ainda é coletado (truncado, não descartado)");
    if (n == 1) {
        checar(lia_strlen(a[0].nome) == LIA_PROC_NOME_MAX - 1, "coletar: nome truncado para exatamente LIA_PROC_NOME_MAX-1 bytes");
        checar(!lia_strncmp(a[0].nome, "0123456789012345678901234567890123456789", LIA_PROC_NOME_MAX - 1),
               "coletar: o prefixo truncado continua correto (nunca lixo/corrupção)");
    }
}

static void teste_coletar_stat_ausente_truncado_invalido(void)
{
    nova_subraiz("malformado");
    char dirp[256];
    caminho(dirp, sizeof dirp, "400", "");
    lia_mkdirat(AT_FDCWD, dirp, 0700);
    /* diretório existe, mas sem "stat" dentro — processo "desapareceu" */

    char statp[256];
    caminho(dirp, sizeof dirp, "401", "");
    lia_mkdirat(AT_FDCWD, dirp, 0700);
    caminho(statp, sizeof statp, "401", "stat");
    escrever_arquivo(statp, "401 (truncado"); /* nem chega a abrir o parêntese */

    caminho(dirp, sizeof dirp, "402", "");
    lia_mkdirat(AT_FDCWD, dirp, 0700);
    caminho(statp, sizeof statp, "402", "stat");
    escrever_arquivo(statp, "402 (ok) S 1 1 1 0 -1 0 0 0 0 0 NAO_NUMERICO 5 20 0\n"); /* utime não numérico */

    caminho(dirp, sizeof dirp, "403", "");
    lia_mkdirat(AT_FDCWD, dirp, 0700);
    caminho(statp, sizeof statp, "403", "stat");
    escrever_arquivo(statp, "403 (parcial) S 1 1 1 0 -1 0 0 0"); /* corta no meio dos campos */

    char statline[512];
    montar_stat(statline, sizeof statline, "404", "este_funciona", 'S', 7, 3);
    criar_processo("404", statline);

    LiaProcAmostra a[16];
    long n = lia_proc_coletar_amostra(g_subraiz, a, 16);
    checar(n == 1, "coletar: stat ausente/truncado/com campo não numérico são todos ignorados, só o válido sobra");
    if (n == 1)
        checar(a[0].pid == 404, "coletar: o único sobrevivente é o pid com stat válido");
}

static void teste_coletar_raiz_inexistente(void)
{
    LiaProcAmostra a[4];
    long n = lia_proc_coletar_amostra("/este/caminho/nao/existe/de/verdade", a, 4);
    checar(n == -1, "coletar: raiz inexistente -> -1 (erro estruturado pelo chamador)");
}

/* lia_proc_coletar_amostra recebe 'raiz' como parâmetro — para testar o
 * limite de memória (parâmetro 'max') não precisamos de uma raiz
 * separada, só de mais fixtures na mesma e um 'max' pequeno na chamada. */
static void teste_coletar_respeita_max(void)
{
    nova_subraiz("limite_memoria");
    for (int i = 500; i < 505; i++) {
        char pidbuf[8], statline[512];
        Buf b; b_init(&b, pidbuf, sizeof pidbuf, -1); b_u(&b, (u64)i); pidbuf[b.n] = 0;
        montar_stat(statline, sizeof statline, pidbuf, "x", 'S', 1, 1);
        criar_processo(pidbuf, statline);
    }
    LiaProcAmostra a[2];
    long n = lia_proc_coletar_amostra(g_subraiz, a, 2);
    checar(n == 2, "coletar: 'max' é respeitado mesmo havendo mais PIDs disponíveis (nunca escreve além dele)");
}

static void teste_calcular_basico(void)
{
    LiaProcAmostra antes[2] = {
        { .pid = 1, .ticks = 100, .estado = 'S', .nome = "a" },
        { .pid = 2, .ticks = 200, .estado = 'S', .nome = "b" },
    };
    LiaProcAmostra depois[2] = {
        { .pid = 1, .ticks = 200, .estado = 'S', .nome = "a" }, /* +100 ticks = 1s de CPU */
        { .pid = 2, .ticks = 200, .estado = 'S', .nome = "b" }, /* +0 ticks */
    };
    LiaProcCpu out[4];
    /* elapsed_ms=1000 (1s real), ncpus=1: pid 1 usou 1s de CPU em 1s de
     * relógio, num só núcleo -> 100.00% exatos -> 10000 centésimos. */
    size_t n = lia_proc_calcular_cpu(antes, 2, depois, 2, 1000, 1, out, 4);
    checar(n == 2, "calcular: as duas amostras (mesmo pid nas duas) produzem dois resultados");
    checar(out[0].pid == 1 && out[0].cpu_centesimos == 10000, "calcular: 1s de CPU em 1s de relógio, 1 núcleo -> 100.00% exatos");
    checar(out[1].pid == 2 && out[1].cpu_centesimos == 0, "calcular: delta zero -> 0.00%");
    checar(out[0].tempo_cpu_centesimos_s == 200, "calcular: tempo_cpu_centesimos_s == ticks finais (LIA_CLK_TCK=100)");
}

static void teste_calcular_normaliza_por_ncpus(void)
{
    LiaProcAmostra antes[1] = { { .pid = 1, .ticks = 0, .estado = 'R', .nome = "a" } };
    LiaProcAmostra depois[1] = { { .pid = 1, .ticks = 100, .estado = 'R', .nome = "a" } }; /* 1s de CPU em 1s real */
    LiaProcCpu out[2];
    size_t n = lia_proc_calcular_cpu(antes, 1, depois, 1, 1000, 4, out, 2);
    checar(n == 1, "calcular (ncpus=4): um resultado");
    checar(out[0].cpu_centesimos == 2500, "calcular: 1 núcleo saturado de 4 -> 25.00% (normalizado pelo total de CPUs)");
}

static void teste_calcular_processo_novo_ou_terminado_ignorado(void)
{
    LiaProcAmostra antes[1] = { { .pid = 1, .ticks = 100, .estado = 'S', .nome = "a" } };
    LiaProcAmostra depois[2] = {
        { .pid = 1, .ticks = 150, .estado = 'S', .nome = "a" },
        { .pid = 99, .ticks = 500, .estado = 'R', .nome = "novo" }, /* só aparece na segunda amostra */
    };
    LiaProcCpu out[4];
    size_t n = lia_proc_calcular_cpu(antes, 1, depois, 2, 1000, 1, out, 4);
    checar(n == 1, "calcular: processo visto só na segunda amostra (novo) é ignorado, não gera delta inventado");
    checar(out[0].pid == 1, "calcular: o processo presente nas duas amostras continua presente no resultado");
}

static void teste_calcular_pid_reciclado_ignorado(void)
{
    /* ticks regredindo para o mesmo pid só pode significar que o pid foi
     * reciclado para outro processo entre as duas amostras — nunca
     * reporta um delta negativo. */
    LiaProcAmostra antes[1] = { { .pid = 7, .ticks = 1000, .estado = 'S', .nome = "velho" } };
    LiaProcAmostra depois[1] = { { .pid = 7, .ticks = 5, .estado = 'S', .nome = "novo_mesmo_pid" } };
    LiaProcCpu out[2];
    size_t n = lia_proc_calcular_cpu(antes, 1, depois, 1, 1000, 1, out, 2);
    checar(n == 0, "calcular: ticks regredindo para o mesmo pid (reciclado) é ignorado, nunca delta negativo");
}

static void teste_calcular_empate_ordena_por_pid(void)
{
    LiaProcAmostra antes[3] = {
        { .pid = 30, .ticks = 0, .estado = 'S', .nome = "c" },
        { .pid = 10, .ticks = 0, .estado = 'S', .nome = "a" },
        { .pid = 20, .ticks = 0, .estado = 'S', .nome = "b" },
    };
    LiaProcAmostra depois[3] = {
        { .pid = 30, .ticks = 50, .estado = 'S', .nome = "c" },
        { .pid = 10, .ticks = 50, .estado = 'S', .nome = "a" }, /* mesmo delta que os outros dois */
        { .pid = 20, .ticks = 50, .estado = 'S', .nome = "b" },
    };
    LiaProcCpu out[4];
    size_t n = lia_proc_calcular_cpu(antes, 3, depois, 3, 1000, 1, out, 4);
    checar(n == 3, "calcular (empate): três resultados");
    checar(out[0].pid == 10 && out[1].pid == 20 && out[2].pid == 30,
           "calcular: cpu_centesimos empatado -> ordem determinística por pid crescente");
}

static void teste_calcular_ordena_decrescente(void)
{
    LiaProcAmostra antes[3] = {
        { .pid = 1, .ticks = 0, .estado = 'S', .nome = "baixo" },
        { .pid = 2, .ticks = 0, .estado = 'S', .nome = "alto" },
        { .pid = 3, .ticks = 0, .estado = 'S', .nome = "medio" },
    };
    LiaProcAmostra depois[3] = {
        { .pid = 1, .ticks = 10, .estado = 'S', .nome = "baixo" },
        { .pid = 2, .ticks = 90, .estado = 'S', .nome = "alto" },
        { .pid = 3, .ticks = 50, .estado = 'S', .nome = "medio" },
    };
    LiaProcCpu out[4];
    size_t n = lia_proc_calcular_cpu(antes, 3, depois, 3, 1000, 1, out, 4);
    checar(n == 3, "calcular (ordenação): três resultados");
    checar(out[0].pid == 2 && out[1].pid == 3 && out[2].pid == 1,
           "calcular: ordenado por cpu_centesimos decrescente (alto, médio, baixo)");
}

static void teste_calcular_max_respeitado(void)
{
    LiaProcAmostra antes[3] = {
        { .pid = 1, .ticks = 0, .estado = 'S', .nome = "a" },
        { .pid = 2, .ticks = 0, .estado = 'S', .nome = "b" },
        { .pid = 3, .ticks = 0, .estado = 'S', .nome = "c" },
    };
    LiaProcAmostra depois[3] = {
        { .pid = 1, .ticks = 10, .estado = 'S', .nome = "a" },
        { .pid = 2, .ticks = 10, .estado = 'S', .nome = "b" },
        { .pid = 3, .ticks = 10, .estado = 'S', .nome = "c" },
    };
    LiaProcCpu out[2];
    size_t n = lia_proc_calcular_cpu(antes, 3, depois, 3, 1000, 1, out, 2);
    checar(n == 2, "calcular: 'max' do resultado é respeitado mesmo com mais processos elegíveis");
}

static void teste_calcular_ncpus_ou_elapsed_zero(void)
{
    LiaProcAmostra antes[1] = { { .pid = 1, .ticks = 0, .estado = 'S', .nome = "a" } };
    LiaProcAmostra depois[1] = { { .pid = 1, .ticks = 10, .estado = 'S', .nome = "a" } };
    LiaProcCpu out[2];
    checar(lia_proc_calcular_cpu(antes, 1, depois, 1, 1000, 0, out, 2) == 0, "calcular: ncpus=0 -> 0 resultados, nunca divide por zero");
    checar(lia_proc_calcular_cpu(antes, 1, depois, 1, 0, 1, out, 2) == 0, "calcular: elapsed_ms=0 -> 0 resultados, nunca divide por zero");
}

long lia_main(long argc, char **argv, char **envp)
{
    (void)argc; (void)envp;
    static char saida_mem[32768];
    b_init(&g_out, saida_mem, sizeof(saida_mem), 1);
    b_init(&lia_out, mem_out, sizeof mem_out, -1);
    b_init(&lia_err, mem_err, sizeof mem_err, -1);

    if (argc < 2) {
        b_s(&g_out, "uso: lia-processos-teste <diretorio_vazio_escrevivel>\n");
        b_flush(&g_out);
        lia_exit(2);
    }
    size_t n = lia_strlen(argv[1]);
    if (n == 0 || n + 1 > sizeof g_raiz) {
        b_s(&g_out, "FALHA caminho de fixture inválido\n");
        b_flush(&g_out);
        lia_exit(1);
    }
    memcpy(g_raiz, argv[1], n);
    g_raiz[n] = 0;

    teste_coletar_basico();
    teste_coletar_nome_com_parenteses();
    teste_coletar_nome_truncado_com_seguranca();
    teste_coletar_stat_ausente_truncado_invalido();
    teste_coletar_raiz_inexistente();
    teste_coletar_respeita_max();
    teste_calcular_basico();
    teste_calcular_normaliza_por_ncpus();
    teste_calcular_processo_novo_ou_terminado_ignorado();
    teste_calcular_pid_reciclado_ignorado();
    teste_calcular_empate_ordena_por_pid();
    teste_calcular_ordena_decrescente();
    teste_calcular_max_respeitado();
    teste_calcular_ncpus_ou_elapsed_zero();

    b_c(&g_out, '\n');
    b_s(&g_out, "lia-processos-teste: "); b_u(&g_out, (u64)g_total); b_s(&g_out, " testes internos, ");
    b_u(&g_out, (u64)g_falhas); b_s(&g_out, " falhas\n");
    b_flush(&g_out);

    lia_exit(g_falhas == 0 ? 0 : 1);
    return 0;
}
