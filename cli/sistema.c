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

    if (json) {
        b_s(&lia_out, "{\"cpu\":{\"nucleos\":"), b_u(&lia_out, (u64)cpus);
        b_s(&lia_out, ",\"modelo\":"), b_json(&lia_out, modelo), b_s(&lia_out, "}");
        b_s(&lia_out, ",\"memoria_kb\":{\"total\":"), b_u(&lia_out, total);
        b_s(&lia_out, ",\"livre\":"), b_u(&lia_out, livre), b_s(&lia_out, ",\"disponivel\":"), b_u(&lia_out, disp);
        b_s(&lia_out, "},\"swap_kb\":{\"total\":"), b_u(&lia_out, swap_t);
        b_s(&lia_out, ",\"livre\":"), b_u(&lia_out, swap_l), b_s(&lia_out, "}");
        b_s(&lia_out, ",\"uptime_s\":"), b_put(&lia_out, up, lup), b_s(&lia_out, "}\n");
    } else {
        b_s(&lia_out, "CPU:    "), b_u(&lia_out, (u64)cpus), b_s(&lia_out, " núcleos ("), b_s(&lia_out, modelo), b_s(&lia_out, ")\n");
        b_s(&lia_out, "RAM:    "), b_u(&lia_out, total / 1024), b_s(&lia_out, " MiB total, ");
        b_u(&lia_out, livre / 1024), b_s(&lia_out, " MiB livre, "), b_u(&lia_out, disp / 1024), b_s(&lia_out, " MiB disponível\n");
        b_s(&lia_out, "Swap:   "), b_u(&lia_out, swap_t / 1024), b_s(&lia_out, " MiB total, ");
        b_u(&lia_out, swap_l / 1024), b_s(&lia_out, " MiB livre\n");
        b_s(&lia_out, "Uptime: "), b_put(&lia_out, up, lup), b_s(&lia_out, " s\n");
    }
    return 0;
}

static int sub_dispositivos(int json)
{
    nD = 0;
    npool = 0;
    listar_dir("/sys/class/net", "rede");
    listar_dir("/sys/class/block", "bloco");
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
        b_u(&lia_out, nD), b_s(&lia_out, " dispositivos (rede/bloco/pci)\n");
    }
    return 0;
}

int cmd_system(int argc, char **argv)
{
    if (argc < 1) {
        b_s(&lia_err, "uso: lia system <diagnosticar|recursos|dispositivos> [--json]\n");
        return 2;
    }
    int json = 0;
    for (int i = 1; i < argc; i++) {
        if (!lia_strcmp(argv[i], "--json")) {
            json = 1;
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
        return sub_dispositivos(json);
    b_s(&lia_err, "lia system: subcomando desconhecido: "), b_s(&lia_err, sub), b_c(&lia_err, '\n');
    return 2;
}
