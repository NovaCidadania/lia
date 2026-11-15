/*
 * lia system — diagnóstico da máquina, enumeração de recursos e dispositivos.
 *
 * Somente leitura: abre apenas arquivos em /proc e /sys, nunca escreve,
 * nunca executa outro programa e nunca pede privilégio. Cada subcomando
 * tem saída humana e --json (payload estável, pensado para ser o
 * `resultado.dados` do protocolo descrito em docs/protocolo.md).
 */
#include "cli.h"

#define MAX_TXT 65536
#define MAX_DISP 512
#define POOL_SIS (64u * 1024)

static char arqtxt[MAX_TXT];
static char pool[POOL_SIS];
static size_t npool;

static const char *dup_(const char *s, size_t n)
{
    if (npool + n + 1 > POOL_SIS)
        return "";
    char *d = pool + npool;
    memcpy(d, s, n);
    d[n] = 0;
    npool += n + 1;
    return d;
}

static long ler(const char *caminho, char *dst, size_t cap)
{
    long fd = lia_openat(AT_FDCWD, caminho, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return fd;
    size_t n = 0;
    for (;;) {
        long r = lia_read((int)fd, dst + n, cap - 1 - n);
        if (r == -EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n == cap - 1)
            break;
    }
    lia_close((int)fd);
    dst[n] = 0;
    return (long)n;
}

/* valor numérico (kB) de uma chave "Nome:   123 kB" dentro de um texto já lido */
static u64 chave_kb(const char *texto, const char *chave)
{
    size_t lc = lia_strlen(chave);
    for (const char *l = texto; *l;) {
        const char *fim = l;
        while (*fim && *fim != '\n')
            fim++;
        if (!lia_strncmp(l, chave, lc) && l[lc] == ':') {
            const char *v = l + lc + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            u64 n = 0;
            while (*v >= '0' && *v <= '9') {
                n = n * 10 + (u64)(*v - '0');
                v++;
            }
            return n;
        }
        l = *fim ? fim + 1 : fim;
    }
    return 0;
}

static const char *distro(void)
{
    long n = ler("/etc/os-release", arqtxt, sizeof arqtxt);
    if (n <= 0)
        return "desconhecida";
    for (char *l = arqtxt; *l;) {
        char *fim = l;
        while (*fim && *fim != '\n')
            fim++;
        if (!lia_strncmp(l, "PRETTY_NAME=", 12)) {
            char *v = l + 12;
            size_t len = (size_t)(fim - v);
            if (len >= 2 && v[0] == '"') {
                v++;
                len -= 2;
            }
            return dup_(v, len);
        }
        l = *fim ? fim + 1 : fim;
    }
    return "desconhecida";
}

static size_t contar_cpus(const char *cpuinfo, const char **modelo)
{
    size_t n = 0;
    *modelo = "desconhecido";
    int achou_modelo = 0;
    for (const char *l = cpuinfo; *l;) {
        const char *fim = l;
        while (*fim && *fim != '\n')
            fim++;
        if (!lia_strncmp(l, "processor", 9) && (l[9] == '\t' || l[9] == ' ' || l[9] == ':'))
            n++;
        if (!achou_modelo && !lia_strncmp(l, "model name", 10)) {
            const char *v = l + 10;
            while (*v == ' ' || *v == '\t' || *v == ':')
                v++;
            *modelo = dup_(v, (size_t)(fim - v));
            achou_modelo = 1;
        }
        l = *fim ? fim + 1 : fim;
    }
    return n;
}

/* ---------- enumeração de dispositivos (somente leitura, sem seguir links) ---------- */

typedef struct {
    const char *nome, *classe, *vendor, *dispositivo;
} Disp;

static Disp D[MAX_DISP];
static size_t nD;

static void listar_dir(const char *dir, const char *classe)
{
    long fd = lia_openat(AT_FDCWD, dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0)
        return;
    static char db[16384] __attribute__((aligned(8)));
    for (;;) {
        long r = lia_getdents64((int)fd, db, sizeof db);
        if (r <= 0)
            break;
        for (long o = 0; o < r;) {
            struct lia_dirent *d = (struct lia_dirent *)(db + o);
            o += d->reclen;
            if (d->nome[0] == '.' && (!d->nome[1] || (d->nome[1] == '.' && !d->nome[2])))
                continue;
            if (nD == MAX_DISP)
                continue;
            D[nD].nome = dup_(d->nome, lia_strlen(d->nome));
            D[nD].classe = classe;
            D[nD].vendor = D[nD].dispositivo = "";
            if (!lia_strcmp(classe, "pci")) {
                char p[512];
                lia_juntar(p, sizeof p, dir, "/", d->nome);
                char p2[560];
                lia_juntar(p2, sizeof p2, p, "/vendor", "");
                char v[32];
                long nv = ler(p2, v, sizeof v);
                while (nv > 0 && (v[nv - 1] == '\n')) nv--;
                D[nD].vendor = dup_(v, (size_t)(nv > 0 ? nv : 0));
                lia_juntar(p2, sizeof p2, p, "/device", "");
                long nd = ler(p2, v, sizeof v);
                while (nd > 0 && (v[nd - 1] == '\n')) nd--;
                D[nD].dispositivo = dup_(v, (size_t)(nd > 0 ? nd : 0));
            }
            nD++;
        }
    }
    lia_close((int)fd);
}

static int ordenar_disp(const void *a, const void *b)
{
    const Disp *x = a, *y = b;
    int c = lia_strcmp(x->classe, y->classe);
    return c ? c : lia_strcmp(x->nome, y->nome);
}

/* ---------- sistema.processos_top_cpu: coleta e cálculo (puros/testáveis) ---------- */

/* USER_HZ: unidade dos campos utime/stime de /proc/<pid>/stat — padrão
 * estável do Linux (proc(5)), independente do HZ interno do kernel.
 * sysconf(_SC_CLK_TCK) sempre retorna 100 em x86_64/Linux na prática;
 * aqui, sem libc, assumimos o valor direto (mesma premissa que `ps`/`top`
 * fazem quando não conseguem perguntar ao kernel). */
#define LIA_CLK_TCK 100

/* "<nome>" é inteiramente dígitos ASCII, não vazio, e não maior que 10
 * caracteres (qualquer pid_max real cabe nisso com folga — um nome
 * "numérico" mais longo que isso nunca é um PID de verdade, e rejeitá-lo
 * evita qualquer risco de overflow na conversão). */
static int nome_e_pid(const char *nome, u32 *pid_saida)
{
    size_t n = lia_strlen(nome);
    if (n == 0 || n > 10)
        return 0;
    u64 v = 0;
    for (size_t i = 0; i < n; i++) {
        if (nome[i] < '0' || nome[i] > '9')
            return 0;
        v = v * 10 + (u64)(nome[i] - '0');
    }
    if (v > 0x7fffffffu)
        return 0;
    *pid_saida = (u32)v;
    return 1;
}

/* Formato de /proc/<pid>/stat (proc(5)): "<pid> (<comm>) <state> <ppid>
 * <pgrp> <session> <tty_nr> <tpgid> <flags> <minflt> <cminflt> <majflt>
 * <cmajflt> <utime> <stime> ...". comm pode conter espaços e até ')' —
 * por isso procura a ÚLTIMA ')' da linha, nunca a primeira depois da
 * primeira '(', para nunca cortar o nome no lugar errado. Alguns campos
 * numéricos (ex.: tpgid, quando não há terminal controlador) podem vir
 * negativos — o sinal é aceito e descartado para os campos que só
 * precisamos pular; nunca para utime/stime, que o kernel nunca reporta
 * negativos. Retorna 1 em sucesso, 0 se o texto não tiver o formato
 * mínimo esperado (truncado, campo não numérico, etc.) — nunca lê além
 * do que 'tam' permite. */
static int parsear_stat(const char *texto, size_t tam, LiaProcAmostra *out)
{
    const char *abre = NULL, *fecha = NULL;
    for (size_t i = 0; i < tam; i++) {
        if (texto[i] == '(' && !abre)
            abre = texto + i;
        if (texto[i] == ')')
            fecha = texto + i; /* mantém a última ocorrência */
    }
    if (!abre || !fecha || fecha <= abre)
        return 0;

    size_t tam_nome = (size_t)(fecha - abre - 1);
    if (tam_nome >= LIA_PROC_NOME_MAX)
        tam_nome = LIA_PROC_NOME_MAX - 1;
    memcpy(out->nome, abre + 1, tam_nome);
    out->nome[tam_nome] = 0;

    const char *fim = texto + tam;
    const char *p = fecha + 1;
    if (p >= fim || *p != ' ')
        return 0;
    while (p < fim && *p == ' ')
        p++;
    if (p >= fim)
        return 0;
    out->estado = *p;
    p++;

    /* a partir daqui: ppid(0) pgrp(1) session(2) tty_nr(3) tpgid(4)
     * flags(5) minflt(6) cminflt(7) majflt(8) cmajflt(9) utime(10)
     * stime(11) — lê 12 campos, guarda só os dois últimos. */
    u64 campo[12];
    for (int i = 0; i < 12; i++) {
        while (p < fim && *p == ' ')
            p++;
        if (p < fim && *p == '-')
            p++; /* sinal descartado — só tpgid costuma vir negativo */
        if (p >= fim || *p < '0' || *p > '9')
            return 0;
        u64 v = 0;
        while (p < fim && *p >= '0' && *p <= '9') {
            v = v * 10 + (u64)(*p - '0');
            p++;
        }
        campo[i] = v;
        if (p < fim && *p == ' ')
            p++;
        else if (i < 11)
            return 0; /* acabou antes da hora: truncado ou malformado */
    }
    out->ticks = campo[10] + campo[11];
    return 1;
}

long lia_proc_coletar_amostra(const char *raiz, LiaProcAmostra *out, size_t max)
{
    long fd = lia_openat(AT_FDCWD, raiz, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    static char db[16384] __attribute__((aligned(8)));
    char caminho_pid[192], caminho_stat[224], conteudo[2048];
    size_t n = 0;
    for (;;) {
        long r = lia_getdents64((int)fd, db, sizeof db);
        if (r <= 0)
            break;
        for (long o = 0; o < r;) {
            struct lia_dirent *d = (struct lia_dirent *)(db + o);
            o += d->reclen;
            u32 pid;
            if (!nome_e_pid(d->nome, &pid))
                continue;
            if (n >= max)
                continue; /* respeita o limite de memória — descarta o excesso, não falha nem transborda */
            if (lia_juntar(caminho_pid, sizeof caminho_pid, raiz, "/", d->nome) < 0)
                continue;
            if (lia_juntar(caminho_stat, sizeof caminho_stat, caminho_pid, "/stat", "") < 0)
                continue;
            long nstat = ler(caminho_stat, conteudo, sizeof conteudo);
            if (nstat <= 0)
                continue; /* processo desapareceu entre listar e ler — ignora, não falha a coleta inteira */
            LiaProcAmostra tmp;
            if (!parsear_stat(conteudo, (size_t)nstat, &tmp))
                continue;
            tmp.pid = pid;
            out[n++] = tmp;
        }
    }
    lia_close((int)fd);
    return (long)n;
}

size_t lia_proc_calcular_cpu(const LiaProcAmostra *antes, size_t n_antes,
                              const LiaProcAmostra *depois, size_t n_depois,
                              u64 elapsed_ms, u64 ncpus,
                              LiaProcCpu *out, size_t max)
{
    size_t n = 0;
    if (ncpus == 0 || elapsed_ms == 0)
        return 0;
    for (size_t i = 0; i < n_depois && n < max; i++) {
        const LiaProcAmostra *a = NULL;
        for (size_t j = 0; j < n_antes; j++) {
            if (antes[j].pid == depois[i].pid) {
                a = &antes[j];
                break;
            }
        }
        if (!a)
            continue; /* processo novo desde a primeira amostra: sem delta confiável */
        if (depois[i].ticks < a->ticks)
            continue; /* ticks nunca regridem para o mesmo processo vivo — só pode ser pid reciclado */
        u64 delta_ticks = depois[i].ticks - a->ticks;

        /* cpu% = (delta_ticks / LIA_CLK_TCK) [s de CPU] / (elapsed_ms/1000) [s decorridos] / ncpus;
         * cpu_centesimos = cpu% * 100. Rearranjado para só multiplicar/dividir inteiros,
         * com arredondamento (+ divisor/2) em vez de truncar: */
        u64 divisor = LIA_CLK_TCK * elapsed_ms * ncpus;
        u64 centesimos = (delta_ticks * 10000000ull + divisor / 2) / divisor;

        out[n].pid = depois[i].pid;
        memcpy(out[n].nome, depois[i].nome, LIA_PROC_NOME_MAX);
        out[n].estado = depois[i].estado;
        out[n].cpu_centesimos = centesimos;
        out[n].tempo_cpu_centesimos_s = depois[i].ticks; /* ticks já são centésimos de segundo (LIA_CLK_TCK=100) */
        n++;
    }

    /* ordena por cpu_centesimos decrescente; empate -> pid crescente
     * (determinístico e testável) — inserção simples, n nunca passa de
     * LIA_PROC_MAX_PROCESSOS nesta etapa. */
    for (size_t i = 1; i < n; i++) {
        LiaProcCpu t = out[i];
        size_t j = i;
        while (j > 0 && (out[j - 1].cpu_centesimos < t.cpu_centesimos ||
                          (out[j - 1].cpu_centesimos == t.cpu_centesimos && out[j - 1].pid > t.pid))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = t;
    }
    return n;
}

static u64 agora_ms_sistema(void)
{
    struct lia_timespec ts;
    lia_clock(1, &ts);
    return (u64)ts.s * 1000 + (u64)ts.ns / 1000000;
}

static void dormir_ms(u32 ms)
{
    u64 alvo = agora_ms_sistema() + ms;
    for (;;) {
        u64 agora = agora_ms_sistema();
        if (agora >= alvo)
            return;
        long r = lia_poll(0, 0, (int)(alvo - agora));
        if (r == -EINTR)
            continue;
        return;
    }
}

static void b_centesimos(Buf *b, u64 centesimos)
{
    b_u(b, centesimos / 100);
    b_c(b, '.');
    u64 frac = centesimos % 100;
    if (frac < 10)
        b_c(b, '0');
    b_u(b, frac);
}

static void b_pad_u(Buf *b, u64 v, size_t largura)
{
    char t[20];
    int i = 20;
    do {
        t[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char s[21];
    size_t n = (size_t)(20 - i);
    memcpy(s, t + i, n);
    s[n] = 0;
    b_pad(b, s, largura);
}

#define LIA_PROC_MAX_PROCESSOS 8192
static LiaProcAmostra g_amostra_a[LIA_PROC_MAX_PROCESSOS];
static LiaProcAmostra g_amostra_b[LIA_PROC_MAX_PROCESSOS];
static LiaProcCpu g_proc_cpu[LIA_PROC_MAX_PROCESSOS];

static int sub_processos_top_cpu(int json, u32 limite, u32 intervalo_ms, const char *raiz_proc)
{
    long ncpuinfo = ler("/proc/cpuinfo", arqtxt, sizeof arqtxt);
    const char *modelo_cpu;
    size_t ncpus = ncpuinfo > 0 ? contar_cpus(arqtxt, &modelo_cpu) : 0;
    if (ncpus == 0)
        ncpus = 1; /* nunca divide por zero; 1 é a suposição mais conservadora */

    long na = lia_proc_coletar_amostra(raiz_proc, g_amostra_a, LIA_PROC_MAX_PROCESSOS);
    if (na < 0) {
        b_s(&lia_err, "lia system processos_top_cpu: não foi possível abrir '");
        b_s(&lia_err, raiz_proc);
        b_s(&lia_err, "' como diretório (/proc não disponível?)\n");
        return 1;
    }

    u64 t0 = agora_ms_sistema();
    dormir_ms(intervalo_ms);
    u64 t1 = agora_ms_sistema();
    u64 elapsed_ms = t1 > t0 ? t1 - t0 : 1; /* nunca zero, mesmo com um relógio adulterado/congelado */

    long nb = lia_proc_coletar_amostra(raiz_proc, g_amostra_b, LIA_PROC_MAX_PROCESSOS);
    if (nb < 0) {
        b_s(&lia_err, "lia system processos_top_cpu: não foi possível abrir '");
        b_s(&lia_err, raiz_proc);
        b_s(&lia_err, "' como diretório (/proc não disponível?)\n");
        return 1;
    }

    size_t ntotal = lia_proc_calcular_cpu(g_amostra_a, (size_t)na, g_amostra_b, (size_t)nb,
                                           elapsed_ms, (u64)ncpus, g_proc_cpu, LIA_PROC_MAX_PROCESSOS);
    size_t nmostrar = ntotal < (size_t)limite ? ntotal : (size_t)limite;

    if (json) {
        b_s(&lia_out, "{\"limite\":"), b_u(&lia_out, limite);
        b_s(&lia_out, ",\"intervalo_ms\":"), b_u(&lia_out, intervalo_ms);
        b_s(&lia_out, ",\"amostrados\":"), b_u(&lia_out, (u64)ntotal);
        b_s(&lia_out, ",\"processos\":[");
        for (size_t i = 0; i < nmostrar; i++) {
            b_s(&lia_out, i ? "," : "");
            b_s(&lia_out, "{\"pid\":"), b_u(&lia_out, g_proc_cpu[i].pid);
            b_s(&lia_out, ",\"nome\":"), b_json(&lia_out, g_proc_cpu[i].nome);
            char est[2] = { g_proc_cpu[i].estado, 0 };
            b_s(&lia_out, ",\"estado\":"), b_json(&lia_out, est);
            /* inteiros, nunca "4.14": o daemon reempacota este JSON
             * passando por json-glib (sessiond/sessiond.c,
             * embutir_json_bruto), que re-interpreta qualquer literal
             * com ponto decimal como double e o perde precisão ao
             * reserializar (ex.: "4.14" -> "4.1399999999999997") — um
             * inteiro em centésimos nunca tem esse problema, por isso
             * "JSON estável" aqui significa centésimos, não ponto
             * flutuante. Dividir por 100 para obter o valor real. */
            b_s(&lia_out, ",\"cpu_pct_centesimos\":"), b_u(&lia_out, g_proc_cpu[i].cpu_centesimos);
            b_s(&lia_out, ",\"tempo_cpu_centesimos_s\":"), b_u(&lia_out, g_proc_cpu[i].tempo_cpu_centesimos_s);
            b_s(&lia_out, "}");
        }
        b_s(&lia_out, "]}\n");
    } else {
        b_pad(&lia_out, "PID", 8);
        b_pad(&lia_out, "NOME", 18);
        b_pad(&lia_out, "EST", 5);
        b_pad(&lia_out, "CPU%", 8);
        b_s(&lia_out, "TEMPO(s)\n");
        for (size_t i = 0; i < nmostrar; i++) {
            b_pad_u(&lia_out, g_proc_cpu[i].pid, 8);
            b_pad(&lia_out, g_proc_cpu[i].nome, 18);
            char est[2] = { g_proc_cpu[i].estado, 0 };
            b_pad(&lia_out, est, 5);
            char cpubuf[16];
            Buf tmp;
            b_init(&tmp, cpubuf, sizeof cpubuf, -1);
            b_centesimos(&tmp, g_proc_cpu[i].cpu_centesimos);
            cpubuf[tmp.n] = 0;
            b_pad(&lia_out, cpubuf, 8);
            b_centesimos(&lia_out, g_proc_cpu[i].tempo_cpu_centesimos_s);
            b_c(&lia_out, '\n');
        }
        b_u(&lia_out, (u64)nmostrar), b_s(&lia_out, " de "), b_u(&lia_out, (u64)ntotal);
        b_s(&lia_out, " processos amostrados (intervalo: "), b_u(&lia_out, intervalo_ms), b_s(&lia_out, " ms)\n");
    }
    return 0;
}

/* ---------- sistema.processos_top_memoria: coleta e ordenação (puras/testáveis) ---------- */

/* Aponta para o valor (depois de "Chave:" e de espaços/tabs) da primeira
 * linha de 'texto' que começa por 'chave', ou NULL se a chave não existir
 * — diferente de chave_kb(), que não distingue "ausente" de "zero" (aqui
 * a diferença importa: thread de kernel não tem VmRSS nenhum). */
static const char *valor_da_chave(const char *texto, const char *chave)
{
    size_t lc = lia_strlen(chave);
    for (const char *l = texto; *l;) {
        const char *fim = l;
        while (*fim && *fim != '\n')
            fim++;
        if (!lia_strncmp(l, chave, lc) && l[lc] == ':') {
            const char *v = l + lc + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            return v;
        }
        l = *fim ? fim + 1 : fim;
    }
    return NULL;
}

/* Fonte escolhida: /proc/<pid>/status (campos Name, State, VmRSS), não
 * /proc/<pid>/statm. Motivo: status já traz o RSS em kB, rotulado pelo
 * próprio kernel; statm traz PÁGINAS, e converter exige o tamanho de
 * página — que, sem libc (sem sysconf/getauxval aqui), teria de ser
 * presumido (4096), premissa falsa em kernels com página de 16K/64K.
 * status também distingue thread de kernel (sem linha VmRSS nenhuma) de
 * processo com RSS zero. Nunca lê cmdline nem environ. */
long lia_proc_coletar_memoria(const char *raiz, LiaProcMem *out, size_t max, u64 *sem_rss)
{
    long fd = lia_openat(AT_FDCWD, raiz, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    static char db[16384] __attribute__((aligned(8)));
    char caminho_pid[192], caminho_status[224], conteudo[4096];
    size_t n = 0;
    u64 n_sem = 0;
    for (;;) {
        long r = lia_getdents64((int)fd, db, sizeof db);
        if (r <= 0)
            break;
        for (long o = 0; o < r;) {
            struct lia_dirent *d = (struct lia_dirent *)(db + o);
            o += d->reclen;
            u32 pid;
            if (!nome_e_pid(d->nome, &pid))
                continue;
            if (lia_juntar(caminho_pid, sizeof caminho_pid, raiz, "/", d->nome) < 0)
                continue;
            if (lia_juntar(caminho_status, sizeof caminho_status, caminho_pid, "/status", "") < 0)
                continue;
            long ns = ler(caminho_status, conteudo, sizeof conteudo);
            if (ns <= 0)
                continue; /* processo desapareceu entre listar e ler — ignora, nunca falha a coleta inteira */
            const char *vnome = valor_da_chave(conteudo, "Name");
            const char *vestado = valor_da_chave(conteudo, "State");
            const char *vrss = valor_da_chave(conteudo, "VmRSS");
            if (!vnome || !vestado || !*vestado || *vestado == '\n')
                continue; /* não tem o formato de um status de verdade */
            if (!vrss) {
                n_sem++; /* thread de kernel (ou zumbi): sem memória de usuário para classificar */
                continue;
            }
            if (*vrss < '0' || *vrss > '9')
                continue; /* VmRSS malformado: descarta só este processo */
            u64 kb = 0;
            while (*vrss >= '0' && *vrss <= '9') {
                kb = kb * 10 + (u64)(*vrss - '0');
                vrss++;
            }
            if (n >= max)
                continue; /* limite fixo de memória — excesso descartado, nunca transborda */
            size_t tn = 0;
            while (vnome[tn] && vnome[tn] != '\n' && tn < LIA_PROC_NOME_MAX - 1)
                tn++;
            memcpy(out[n].nome, vnome, tn);
            out[n].nome[tn] = 0;
            out[n].pid = pid;
            out[n].estado = *vestado;
            out[n].rss_kb = kb;
            out[n].mem_centesimos = 0;
            n++;
        }
    }
    lia_close((int)fd);
    if (sem_rss)
        *sem_rss = n_sem;
    return (long)n;
}

void lia_proc_ordenar_memoria(LiaProcMem *v, size_t n, u64 mem_total_kb)
{
    for (size_t i = 0; i < n; i++)
        v[i].mem_centesimos = mem_total_kb ? (v[i].rss_kb * 10000ull + mem_total_kb / 2) / mem_total_kb : 0;
    /* RSS decrescente; empate -> pid crescente (determinístico e testável) */
    for (size_t i = 1; i < n; i++) {
        LiaProcMem t = v[i];
        size_t j = i;
        while (j > 0 && (v[j - 1].rss_kb < t.rss_kb || (v[j - 1].rss_kb == t.rss_kb && v[j - 1].pid > t.pid))) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = t;
    }
}

static LiaProcMem g_proc_mem[LIA_PROC_MAX_PROCESSOS];

static int sub_processos_top_memoria(int json, u32 limite, const char *raiz_proc)
{
    u64 sem_rss = 0;
    long n = lia_proc_coletar_memoria(raiz_proc, g_proc_mem, LIA_PROC_MAX_PROCESSOS, &sem_rss);
    if (n < 0) {
        b_s(&lia_err, "lia system processos_top_memoria: não foi possível abrir '");
        b_s(&lia_err, raiz_proc);
        b_s(&lia_err, "' como diretório (/proc não disponível?)\n");
        return 1;
    }
    char caminho_meminfo[224];
    u64 total_kb = 0;
    if (lia_juntar(caminho_meminfo, sizeof caminho_meminfo, raiz_proc, "/meminfo", "") == 0) {
        long nm = ler(caminho_meminfo, arqtxt, sizeof arqtxt);
        total_kb = nm > 0 ? chave_kb(arqtxt, "MemTotal") : 0;
    }
    lia_proc_ordenar_memoria(g_proc_mem, (size_t)n, total_kb);
    size_t nmostrar = (size_t)n < (size_t)limite ? (size_t)n : (size_t)limite;

    if (json) {
        /* só inteiros (kB e centésimos de %), nunca ponto flutuante —
         * mesmo motivo de cpu_pct_centesimos em processos_top_cpu. */
        b_s(&lia_out, "{\"ordenar_por\":\"memoria\",\"limite\":"), b_u(&lia_out, limite);
        b_s(&lia_out, ",\"considerados\":"), b_u(&lia_out, (u64)n);
        b_s(&lia_out, ",\"sem_memoria_de_usuario\":"), b_u(&lia_out, sem_rss);
        b_s(&lia_out, ",\"memoria_total_kb\":"), b_u(&lia_out, total_kb);
        b_s(&lia_out, ",\"processos\":[");
        for (size_t i = 0; i < nmostrar; i++) {
            b_s(&lia_out, i ? "," : "");
            b_s(&lia_out, "{\"pid\":"), b_u(&lia_out, g_proc_mem[i].pid);
            b_s(&lia_out, ",\"nome\":"), b_json(&lia_out, g_proc_mem[i].nome);
            char est[2] = { g_proc_mem[i].estado, 0 };
            b_s(&lia_out, ",\"estado\":"), b_json(&lia_out, est);
            b_s(&lia_out, ",\"rss_kb\":"), b_u(&lia_out, g_proc_mem[i].rss_kb);
            b_s(&lia_out, ",\"mem_pct_centesimos\":"), b_u(&lia_out, g_proc_mem[i].mem_centesimos);
            b_s(&lia_out, "}");
        }
        b_s(&lia_out, "]}\n");
    } else {
        b_pad(&lia_out, "PID", 8);
        b_pad(&lia_out, "NOME", 18);
        b_pad(&lia_out, "EST", 5);
        b_pad(&lia_out, "RSS(MiB)", 10);
        b_s(&lia_out, "MEM%\n");
        for (size_t i = 0; i < nmostrar; i++) {
            b_pad_u(&lia_out, g_proc_mem[i].pid, 8);
            b_pad(&lia_out, g_proc_mem[i].nome, 18);
            char est[2] = { g_proc_mem[i].estado, 0 };
            b_pad(&lia_out, est, 5);
            char buf[24];
            Buf tmp;
            b_init(&tmp, buf, sizeof buf, -1);
            b_centesimos(&tmp, (g_proc_mem[i].rss_kb * 100 + 512) / 1024);
            buf[tmp.n] = 0;
            b_pad(&lia_out, buf, 10);
            b_centesimos(&lia_out, g_proc_mem[i].mem_centesimos);
            b_c(&lia_out, '\n');
        }
        b_u(&lia_out, (u64)nmostrar), b_s(&lia_out, " de "), b_u(&lia_out, (u64)n);
        b_s(&lia_out, " processos com memória de usuário ("), b_u(&lia_out, sem_rss);
        b_s(&lia_out, " sem VmRSS, ex.: threads de kernel); RAM total: ");
        b_u(&lia_out, total_kb / 1024), b_s(&lia_out, " MiB\n");
    }
    return 0;
}

/* ---------- subcomandos ---------- */

static int sub_diagnosticar(int json)
{
    struct lia_utsname u;
    memset(&u, 0, sizeof u);
    lia_uname(&u);
    long nmem = ler("/proc/meminfo", arqtxt, sizeof arqtxt);
    u64 memtotal = nmem > 0 ? chave_kb(arqtxt, "MemTotal") : 0;
    u64 memdisp = nmem > 0 ? chave_kb(arqtxt, "MemAvailable") : 0;
    long ncpu = ler("/proc/cpuinfo", arqtxt, sizeof arqtxt);
    const char *modelo;
    size_t cpus = ncpu > 0 ? contar_cpus(arqtxt, &modelo) : 0;
    const char *d = distro();

    if (json) {
        b_s(&lia_out, "{\"distro\":"), b_json(&lia_out, d);
        b_s(&lia_out, ",\"kernel\":{\"sysname\":"), b_json(&lia_out, u.sysname);
        b_s(&lia_out, ",\"release\":"), b_json(&lia_out, u.release);
        b_s(&lia_out, ",\"machine\":"), b_json(&lia_out, u.machine), b_s(&lia_out, "}");
        b_s(&lia_out, ",\"cpu\":{\"nucleos\":"), b_u(&lia_out, (u64)cpus);
        b_s(&lia_out, ",\"modelo\":"), b_json(&lia_out, modelo), b_s(&lia_out, "}");
        b_s(&lia_out, ",\"memoria_kb\":{\"total\":"), b_u(&lia_out, memtotal);
        b_s(&lia_out, ",\"disponivel\":"), b_u(&lia_out, memdisp), b_s(&lia_out, "}");
        b_s(&lia_out, ",\"usuario\":{\"uid\":"), b_u(&lia_out, (u64)lia_getuid());
        b_s(&lia_out, ",\"euid\":"), b_u(&lia_out, (u64)lia_geteuid()), b_s(&lia_out, "}}\n");
    } else {
        b_s(&lia_out, "Sistema:   "), b_s(&lia_out, d), b_c(&lia_out, '\n');
        b_s(&lia_out, "Kernel:    "), b_s(&lia_out, u.sysname), b_c(&lia_out, ' ');
        b_s(&lia_out, u.release), b_c(&lia_out, ' '), b_s(&lia_out, u.machine), b_c(&lia_out, '\n');
        b_s(&lia_out, "CPU:       "), b_u(&lia_out, (u64)cpus), b_s(&lia_out, " núcleos, ");
        b_s(&lia_out, modelo), b_c(&lia_out, '\n');
        b_s(&lia_out, "Memória:   "), b_u(&lia_out, memtotal / 1024), b_s(&lia_out, " MiB total, ");
        b_u(&lia_out, memdisp / 1024), b_s(&lia_out, " MiB disponível\n");
        b_s(&lia_out, "Usuário:   uid "), b_u(&lia_out, (u64)lia_getuid());
        b_s(&lia_out, ", euid "), b_u(&lia_out, (u64)lia_geteuid()), b_c(&lia_out, '\n');
    }
    return 0;
}

static int sub_recursos(int json)
{
    long nmem = ler("/proc/meminfo", arqtxt, sizeof arqtxt);
    u64 total = nmem > 0 ? chave_kb(arqtxt, "MemTotal") : 0;
    u64 livre = nmem > 0 ? chave_kb(arqtxt, "MemFree") : 0;
    u64 disp = nmem > 0 ? chave_kb(arqtxt, "MemAvailable") : 0;
    u64 swap_t = nmem > 0 ? chave_kb(arqtxt, "SwapTotal") : 0;
    u64 swap_l = nmem > 0 ? chave_kb(arqtxt, "SwapFree") : 0;
    long ncpu = ler("/proc/cpuinfo", arqtxt, sizeof arqtxt);
    const char *modelo;
    size_t cpus = ncpu > 0 ? contar_cpus(arqtxt, &modelo) : 0;
    char up[256];
    long nup = ler("/proc/uptime", up, sizeof up);
    size_t lup = 0;
    while ((size_t)lup < (size_t)(nup > 0 ? nup : 0) && up[lup] != ' ')
        lup++;
    u64 uptime_int = 0;
    for (size_t i = 0; i < lup && up[i] >= '0' && up[i] <= '9'; i++)
        uptime_int = uptime_int * 10 + (u64)(up[i] - '0');
    /* /proc/loadavg: "0.52 0.58 0.59 1/1234 56789" — as três médias sempre
     * com duas casas; guardadas em centésimos (inteiro, nunca float — ver
     * cpu_pct_centesimos). carga_ok=0 se o arquivo não existir ou não
     * tiver esse formato: o campo simplesmente não é emitido (nunca um
     * zero inventado). */
    char la[128];
    long nla = ler("/proc/loadavg", la, sizeof la);
    u64 carga[3] = { 0, 0, 0 };
    int carga_ok = 0;
    if (nla > 0) {
        const char *p = la;
        int k = 0;
        for (; k < 3; k++) {
            u64 inteiro = 0, frac = 0;
            int nd = 0, nf = 0;
            while (*p >= '0' && *p <= '9') { inteiro = inteiro * 10 + (u64)(*p - '0'); p++; nd++; }
            if (!nd || *p != '.')
                break;
            p++;
            while (*p >= '0' && *p <= '9' && nf < 2) { frac = frac * 10 + (u64)(*p - '0'); p++; nf++; }
            if (nf != 2)
                break;
            while (*p >= '0' && *p <= '9')
                p++;
            carga[k] = inteiro * 100 + frac;
            if (*p == ' ')
                p++;
        }
        carga_ok = k == 3;
    }

    if (json) {
        b_s(&lia_out, "{\"cpu\":{\"nucleos\":"), b_u(&lia_out, (u64)cpus);
        b_s(&lia_out, ",\"modelo\":"), b_json(&lia_out, modelo), b_s(&lia_out, "}");
        b_s(&lia_out, ",\"memoria_kb\":{\"total\":"), b_u(&lia_out, total);
        b_s(&lia_out, ",\"livre\":"), b_u(&lia_out, livre), b_s(&lia_out, ",\"disponivel\":"), b_u(&lia_out, disp);
        b_s(&lia_out, "},\"swap_kb\":{\"total\":"), b_u(&lia_out, swap_t);
        b_s(&lia_out, ",\"livre\":"), b_u(&lia_out, swap_l), b_s(&lia_out, "}");
        b_s(&lia_out, ",\"uptime_s\":"), b_put(&lia_out, up, lup);
        b_s(&lia_out, ",\"uptime_segundos\":"), b_u(&lia_out, uptime_int);
        if (carga_ok) {
            b_s(&lia_out, ",\"carga_media_centesimos\":{\"1min\":"), b_u(&lia_out, carga[0]);
            b_s(&lia_out, ",\"5min\":"), b_u(&lia_out, carga[1]);
            b_s(&lia_out, ",\"15min\":"), b_u(&lia_out, carga[2]), b_s(&lia_out, "}");
        }
        b_s(&lia_out, "}\n");
    } else {
        b_s(&lia_out, "CPU:    "), b_u(&lia_out, (u64)cpus), b_s(&lia_out, " núcleos ("), b_s(&lia_out, modelo), b_s(&lia_out, ")\n");
        b_s(&lia_out, "RAM:    "), b_u(&lia_out, total / 1024), b_s(&lia_out, " MiB total, ");
        b_u(&lia_out, livre / 1024), b_s(&lia_out, " MiB livre, "), b_u(&lia_out, disp / 1024), b_s(&lia_out, " MiB disponível\n");
        b_s(&lia_out, "Swap:   "), b_u(&lia_out, swap_t / 1024), b_s(&lia_out, " MiB total, ");
        b_u(&lia_out, swap_l / 1024), b_s(&lia_out, " MiB livre\n");
        b_s(&lia_out, "Uptime: "), b_put(&lia_out, up, lup), b_s(&lia_out, " s\n");
        if (carga_ok) {
            b_s(&lia_out, "Carga:  ");
            b_centesimos(&lia_out, carga[0]), b_c(&lia_out, ' ');
            b_centesimos(&lia_out, carga[1]), b_c(&lia_out, ' ');
            b_centesimos(&lia_out, carga[2]), b_s(&lia_out, " (média de 1, 5 e 15 min)\n");
        }
    }
    return 0;
}

static int sub_dispositivos(int json, const char *classe)
{
    nD = 0;
    npool = 0;
    /* 'classe' NULL = as três; senão só a pedida (já validada em cmd_system) */
    if (!classe || !lia_strcmp(classe, "rede"))
        listar_dir("/sys/class/net", "rede");
    if (!classe || !lia_strcmp(classe, "bloco"))
        listar_dir("/sys/class/block", "bloco");
    if (!classe || !lia_strcmp(classe, "pci"))
        listar_dir("/sys/bus/pci/devices", "pci");
    /* ordenação simples (poucas centenas de itens) */
    for (size_t i = 1; i < nD; i++) {
        Disp t = D[i];
        size_t j = i;
        while (j > 0 && ordenar_disp(&D[j - 1], &t) > 0) {
            D[j] = D[j - 1];
            j--;
        }
        D[j] = t;
    }
    if (json) {
        b_s(&lia_out, "[");
        for (size_t i = 0; i < nD; i++) {
            b_s(&lia_out, i ? ",\n  " : "\n  ");
            b_s(&lia_out, "{\"classe\":"), b_json(&lia_out, D[i].classe);
            b_s(&lia_out, ",\"nome\":"), b_json(&lia_out, D[i].nome);
            if (D[i].vendor[0] || D[i].dispositivo[0]) {
                b_s(&lia_out, ",\"vendor\":"), b_json(&lia_out, D[i].vendor);
                b_s(&lia_out, ",\"dispositivo\":"), b_json(&lia_out, D[i].dispositivo);
            }
            b_s(&lia_out, "}");
        }
        b_s(&lia_out, nD ? "\n]\n" : "]\n");
    } else {
        for (size_t i = 0; i < nD; i++) {
            b_pad(&lia_out, D[i].classe, 8);
            b_s(&lia_out, D[i].nome);
            if (D[i].vendor[0]) {
                b_s(&lia_out, "  vendor="), b_s(&lia_out, D[i].vendor);
                b_s(&lia_out, " dispositivo="), b_s(&lia_out, D[i].dispositivo);
            }
            b_c(&lia_out, '\n');
        }
        b_u(&lia_out, nD), b_s(&lia_out, " dispositivos ("), b_s(&lia_out, classe ? classe : "rede/bloco/pci"), b_s(&lia_out, ")\n");
    }
    return 0;
}

/* Converte um inteiro decimal sem sinal (ex.: valor de --limite). -1 se
 * 's' for vazio, tiver qualquer caractere não numérico, ou passar de
 * 2^32-1 — nunca trunca silenciosamente um valor fora da faixa. */
static long parsear_u32(const char *s)
{
    if (!s || !*s)
        return -1;
    u64 v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10 + (u64)(*p - '0');
        if (v > 0xffffffffull)
            return -1;
    }
    return (long)v;
}

int cmd_system(int argc, char **argv)
{
    if (argc < 1) {
        b_s(&lia_err, "uso: lia system <diagnosticar|recursos|dispositivos|processos_top_cpu|processos_top_memoria> [--json]\n"
                       "                [--limite N] [--intervalo-ms N] [--ordenar-por cpu|memoria] [--classe rede|bloco|pci]\n"
                       "                [--raiz-proc CAMINHO]\n");
        return 2;
    }
    int json = 0;
    u32 limite = 10, intervalo_ms = 200;
    const char *raiz_proc = "/proc";
    const char *ordenar_por = NULL, *classe = NULL;
    for (int i = 1; i < argc; i++) {
        if (!lia_strcmp(argv[i], "--json")) {
            json = 1;
        } else if (!lia_strcmp(argv[i], "--limite") && i + 1 < argc) {
            long v = parsear_u32(argv[++i]);
            if (v < 0) {
                b_s(&lia_err, "lia system: --limite precisa ser um inteiro não negativo\n");
                return 2;
            }
            limite = (u32)v;
        } else if (!lia_strcmp(argv[i], "--intervalo-ms") && i + 1 < argc) {
            long v = parsear_u32(argv[++i]);
            if (v < 0) {
                b_s(&lia_err, "lia system: --intervalo-ms precisa ser um inteiro não negativo\n");
                return 2;
            }
            intervalo_ms = (u32)v;
        } else if (!lia_strcmp(argv[i], "--raiz-proc") && i + 1 < argc) {
            /* só para diagnóstico/teste (testes/nucleo/) — nunca a
             * resposta normal de produção, que sempre usa "/proc". */
            raiz_proc = argv[++i];
        } else if (!lia_strcmp(argv[i], "--ordenar-por") && i + 1 < argc) {
            ordenar_por = argv[++i];
            if (lia_strcmp(ordenar_por, "cpu") && lia_strcmp(ordenar_por, "memoria")) {
                b_s(&lia_err, "lia system: --ordenar-por só aceita 'cpu' (processos_top_cpu) ou 'memoria' (processos_top_memoria)\n");
                return 2;
            }
        } else if (!lia_strcmp(argv[i], "--classe") && i + 1 < argc) {
            classe = argv[++i];
            if (lia_strcmp(classe, "rede") && lia_strcmp(classe, "bloco") && lia_strcmp(classe, "pci")) {
                b_s(&lia_err, "lia system: --classe só aceita 'rede', 'bloco' ou 'pci'\n");
                return 2;
            }
        } else {
            b_s(&lia_err, "lia system: opção desconhecida: "), b_s(&lia_err, argv[i]), b_c(&lia_err, '\n');
            return 2;
        }
    }
    const char *sub = argv[0];
    if (!lia_strcmp(sub, "diagnosticar"))
        return sub_diagnosticar(json);
    if (!lia_strcmp(sub, "recursos"))
        return sub_recursos(json);
    if (!lia_strcmp(sub, "dispositivos"))
        return sub_dispositivos(json, classe);
    if (!lia_strcmp(sub, "processos_top_memoria")) {
        if (limite < 1 || limite > 100) {
            b_s(&lia_err, "lia system: --limite precisa estar entre 1 e 100\n");
            return 2;
        }
        if (ordenar_por && lia_strcmp(ordenar_por, "memoria")) {
            b_s(&lia_err, "lia system: processos_top_memoria só ordena por 'memoria'\n");
            return 2;
        }
        return sub_processos_top_memoria(json, limite, raiz_proc);
    }
    if (!lia_strcmp(sub, "processos_top_cpu")) {
        if (ordenar_por && lia_strcmp(ordenar_por, "cpu")) {
            b_s(&lia_err, "lia system: processos_top_cpu só ordena por 'cpu'\n");
            return 2;
        }
        if (limite < 1 || limite > 100) {
            b_s(&lia_err, "lia system: --limite precisa estar entre 1 e 100\n");
            return 2;
        }
        if (intervalo_ms < 100 || intervalo_ms > 5000) {
            b_s(&lia_err, "lia system: --intervalo-ms precisa estar entre 100 e 5000\n");
            return 2;
        }
        return sub_processos_top_cpu(json, limite, intervalo_ms, raiz_proc);
    }
    b_s(&lia_err, "lia system: subcomando desconhecido: "), b_s(&lia_err, sub), b_c(&lia_err, '\n');
    return 2;
}
