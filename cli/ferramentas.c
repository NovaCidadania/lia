/*
 * lia tools — descoberta, inventário e probes seguros de executáveis.
 *
 * Fontes: PATH (na ordem), diretórios convencionais e ~/.local/bin.
 * Persistência: ~/.lia/maquina/inventario/ferramentas.lia (binário versionado)
 *               ~/.lia/humano/diagnosticos/ferramentas.md  (legível)
 *
 * Nunca usa shell nem sudo. Probes só rodam o que está no catálogo, com
 * argumentos inofensivos, stdin=/dev/null, ambiente mínimo, timeout e
 * limite de saída.
 */
#include "cli.h"

#define MAX_FERR 16384
#define MAX_DIRS 128
#define POOL_TAM (4u << 20)
#define ARQ_TAM (8u << 20)
#define HASH_TAM 32768 /* potência de 2, > 2 * MAX_FERR */
#define PROBE_LIMITE_MS 3000

enum {
    E_DESCOBERTA,
    E_ACESSIVEL,
    E_PROBE_OK,
    E_PROBE_FALHOU,
    E_SEM_PERMISSAO,
    E_PRECISA_ENTRADA,
    E_SEM_PROBE,
    E_N
};
static const char *const NOME_ESTADO[E_N] = {
    "DISCOVERED", "ACCESSIBLE", "PROBE_SUCCESS", "PROBE_FAILED",
    "PERMISSION_DENIED", "NEEDS_INPUT", "NO_SAFE_PROBE",
};

enum { T_OUTRO, T_ELF, T_SCRIPT, T_VAZIO };
static const char *const NOME_TIPO[] = {"outro", "ELF", "script", "vazio"};

typedef struct {
    const char *nome, *dir, *alvo, *info, *versao;
    u32 mode, uid, gid, dur_ms;
    u64 size;
    i32 saida;
    u16 maq;
    u8 tipo, simlink, estado;
} Ferr;

static Ferr F[MAX_FERR];
static size_t nF;
static char pool[POOL_TAM];
static size_t npool;
static char arq[ARQ_TAM];     /* inventário carregado do disco */
static char saidabuf[ARQ_TAM]; /* inventário/markdown em construção */
static u32 quebrados, sombreadas, nao_exec, truncado;

static u64 dir_dev[MAX_DIRS], dir_ino[MAX_DIRS];
static size_t ndirs;
static i32 htab[HASH_TAM]; /* índice+1 em F; 0 = vazio */

static i64 data_inventario;

/* ---------- utilitários ---------- */

static const char *pool_dup(const char *s, size_t n)
{
    if (npool + n + 1 > POOL_TAM) {
        truncado = 1;
        return "";
    }
    char *d = pool + npool;
    memcpy(d, s, n);
    d[n] = 0;
    npool += n + 1;
    return d;
}

static u64 fnv(const void *p, size_t n, u64 h)
{
    const unsigned char *c = p;
    while (n--) {
        h ^= *c++;
        h *= 1099511628211ull;
    }
    return h;
}
#define FNV_INI 14695981039346656037ull

static u64 agora_ms(void)
{
    struct lia_timespec ts;
    lia_clock(1, &ts);
    return (u64)ts.s * 1000 + (u64)ts.ns / 1000000;
}

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

static long ler_arquivo(const char *caminho, char *dst, size_t cap)
{
    long fd = lia_openat(AT_FDCWD, caminho, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return fd;
    size_t n = 0;
    for (;;) {
        long r = lia_read((int)fd, dst + n, cap - n);
        if (r == -EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n == cap)
            break;
    }
    lia_close((int)fd);
    return (long)n;
}

static int gravar_arquivo(const char *caminho, const char *dados, size_t n)
{
    char tmp[4200];
    if (lia_juntar(tmp, sizeof tmp, caminho, ".tmp", "") < 0)
        return -1;
    long fd = lia_openat(AT_FDCWD, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return (int)fd;
    int rc = lia_escrever_tudo((int)fd, dados, n);
    lia_close((int)fd);
    if (rc < 0)
        return -5;
    return (int)lia_rename(tmp, caminho);
}

/* ---------- ~/.lia ---------- */

static char p_home[1024], p_inv[1100], p_md[1100];

int lia_preparar(void)
{
    const char *home = lia_getenv("HOME");
    if (!home || !*home || lia_strlen(home) > 900) {
        b_s(&lia_err, "lia: HOME não definido\n");
        return -1;
    }
    char p[1200];
    static const char *const sub[] = {
        "/.lia", "/.lia/humano", "/.lia/humano/diagnosticos", "/.lia/humano/auditoria",
        "/.lia/maquina", "/.lia/maquina/inventario",
    };
    for (size_t i = 0; i < sizeof sub / sizeof *sub; i++) {
        lia_juntar(p, sizeof p, home, sub[i], "");
        long r = lia_mkdirat(AT_FDCWD, p, 0700);
        if (r < 0 && r != -EEXIST) {
            b_s(&lia_err, "lia: não foi possível criar ");
            b_s(&lia_err, p);
            b_s(&lia_err, " (errno ");
            b_u(&lia_err, (u64)-r);
            b_s(&lia_err, ")\n");
            return -1;
        }
    }
    lia_juntar(p_home, sizeof p_home, home, "", "");
    lia_juntar(p_inv, sizeof p_inv, home, "/.lia/maquina/inventario/ferramentas.lia", "");
    lia_juntar(p_md, sizeof p_md, home, "/.lia/humano/diagnosticos/ferramentas.md", "");
    return 0;
}

const char *lia_home(void) { return p_home; }

/* ---------- descoberta ---------- */

static u32 hash_nome(const char *s)
{
    return (u32)(fnv(s, lia_strlen(s), FNV_INI) & (HASH_TAM - 1));
}

static int achar(const char *nome)
{
    for (u32 h = hash_nome(nome);; h = (h + 1) & (HASH_TAM - 1)) {
        if (!htab[h])
            return -1;
        if (!lia_strcmp(F[htab[h] - 1].nome, nome))
            return htab[h] - 1;
    }
}

static void inserir_hash(const char *nome, int idx)
{
    u32 h = hash_nome(nome);
    while (htab[h])
        h = (h + 1) & (HASH_TAM - 1);
    htab[h] = idx + 1;
}

static void classificar(Ferr *f, int dirfd)
{
    f->tipo = T_OUTRO;
    if (f->size == 0) {
        f->tipo = T_VAZIO;
        return;
    }
    long fd = lia_openat(dirfd, f->nome, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return; /* exec sem leitura: tipo desconhecido */
    unsigned char h[64];
    long n = lia_read((int)fd, h, sizeof h);
    lia_close((int)fd);
    if (n >= 20 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
        f->tipo = T_ELF;
        f->maq = (u16)(h[18] | (h[19] << 8));
    } else if (n >= 2 && h[0] == '#' && h[1] == '!') {
        f->tipo = T_SCRIPT;
        size_t l = 2;
        while (l < (size_t)n && h[l] != '\n')
            l++;
        f->info = pool_dup((const char *)h + 2, l - 2);
    }
}

static void processar(int dirfd, const char *dir, const char *nome)
{
    struct lia_stat st;
    if (lia_fstatat(dirfd, nome, &st, 0) < 0) {
        quebrados++;
        return;
    }
    if ((st.mode & S_IFMT) != S_IFREG)
        return;
    if (!(st.mode & 0111)) {
        nao_exec++; /* arquivo regular sem bit de execução: não é ferramenta */
        return;
    }
    /* Como execvp: uma entrada sem permissão não esconde uma acessível mais adiante no PATH. */
    int acessivel = lia_faccessat2(dirfd, nome, 1, AT_EACCESS) == 0;
    int idx = achar(nome);
    if (idx >= 0 && !(F[idx].estado == E_SEM_PERMISSAO && acessivel)) {
        sombreadas++;
        return;
    }
    if (idx < 0 && nF == MAX_FERR) {
        truncado = 1;
        return;
    }
    Ferr *f = &F[idx >= 0 ? (size_t)idx : nF];
    memset(f, 0, sizeof *f);
    f->nome = pool_dup(nome, lia_strlen(nome));
    f->dir = dir;
    f->alvo = f->info = f->versao = "";
    f->mode = st.mode;
    f->uid = st.uid;
    f->gid = st.gid;
    f->size = (u64)st.size;
    f->saida = -1;

    struct lia_stat ls;
    if (lia_fstatat(dirfd, nome, &ls, AT_SYMLINK_NOFOLLOW) == 0 && (ls.mode & S_IFMT) == S_IFLNK) {
        char alvo[4096];
        long r = lia_readlinkat(dirfd, nome, alvo, sizeof alvo - 1);
        f->simlink = 1;
        if (r > 0)
            f->alvo = pool_dup(alvo, (size_t)r);
    }
    f->estado = acessivel ? E_ACESSIVEL : E_SEM_PERMISSAO;
    classificar(f, dirfd);
    if (idx < 0) {
        inserir_hash(f->nome, (int)nF);
        nF++;
    }
}

static void varrer_dir(const char *dir)
{
    if (!*dir)
        return;
    struct lia_stat ds;
    if (lia_fstatat(AT_FDCWD, dir, &ds, 0) < 0 || (ds.mode & S_IFMT) != 0040000)
        return;
    for (size_t i = 0; i < ndirs; i++)
        if (dir_dev[i] == ds.dev && dir_ino[i] == ds.ino)
            return; /* mesmo diretório real (ex.: /bin -> usr/bin) */
    if (ndirs == MAX_DIRS)
        return;
    dir_dev[ndirs] = ds.dev;
    dir_ino[ndirs++] = ds.ino;

    long fd = lia_openat(AT_FDCWD, dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd < 0)
        return;
    const char *dpool = pool_dup(dir, lia_strlen(dir));
    static char db[32768] __attribute__((aligned(8)));
    for (;;) {
        long r = lia_getdents64((int)fd, db, sizeof db);
        if (r <= 0)
            break;
        for (long o = 0; o < r;) {
            struct lia_dirent *d = (struct lia_dirent *)(db + o);
            o += d->reclen;
            if (d->nome[0] == '.' && (!d->nome[1] || (d->nome[1] == '.' && !d->nome[2])))
                continue;
            processar((int)fd, dpool, d->nome);
        }
    }
    lia_close((int)fd);
}

static void ordenar(void)
{
    /* shell sort por nome */
    for (size_t gap = nF / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < nF; i++) {
            Ferr t = F[i];
            size_t j = i;
            while (j >= gap && lia_strcmp(F[j - gap].nome, t.nome) > 0) {
                F[j] = F[j - gap];
                j -= gap;
            }
            F[j] = t;
        }
}

static void varrer_tudo(void)
{
    char comp[4096];
    const char *path = lia_getenv("PATH");
    while (path && *path) {
        size_t n = 0;
        while (path[n] && path[n] != ':')
            n++;
        if (n > 0 && n < sizeof comp) {
            memcpy(comp, path, n);
            comp[n] = 0;
            varrer_dir(comp);
        }
        path += n;
        if (*path == ':')
            path++;
    }
    static const char *const conv[] = {
        "/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin", "/sbin", "/bin",
    };
    for (size_t i = 0; i < sizeof conv / sizeof *conv; i++)
        varrer_dir(conv[i]);
    if (lia_juntar(comp, sizeof comp, p_home, "/.local/bin", "") == 0)
        varrer_dir(comp);
    ordenar();
}

/* ---------- catálogo de probes seguros ---------- */

static const struct {
    const char *nome, *arg;
} CATALOGO[] = {
    {"ls", "--version"}, {"cat", "--version"}, {"echo", "--version"}, {"date", "--version"},
    {"uname", "--version"}, {"id", "--version"}, {"whoami", "--version"}, {"pwd", "--version"},
    {"head", "--version"}, {"tail", "--version"}, {"wc", "--version"}, {"sort", "--version"},
    {"uniq", "--version"}, {"tr", "--version"}, {"cut", "--version"}, {"tee", "--version"},
    {"cp", "--version"}, {"mv", "--version"}, {"rm", "--version"}, {"mkdir", "--version"},
    {"rmdir", "--version"}, {"ln", "--version"}, {"chmod", "--version"}, {"chown", "--version"},
    {"touch", "--version"}, {"df", "--version"}, {"du", "--version"}, {"stat", "--version"},
    {"basename", "--version"}, {"dirname", "--version"}, {"env", "--version"},
    {"printenv", "--version"}, {"sleep", "--version"}, {"true", "--version"}, {"false", "--version"},
    {"nproc", "--version"}, {"tty", "--version"}, {"uptime", "--version"}, {"hostname", "--version"},
    {"grep", "--version"}, {"sed", "--version"}, {"awk", "--version"}, {"find", "--version"},
    {"xargs", "--version"}, {"tar", "--version"}, {"gzip", "--version"}, {"bzip2", "-V"},
    {"xz", "--version"}, {"zstd", "--version"}, {"curl", "--version"}, {"wget", "--version"},
    {"git", "--version"}, {"gcc", "--version"}, {"cc", "--version"}, {"ld", "--version"},
    {"as", "--version"}, {"make", "--version"}, {"ssh", "-V"}, {"sudo", "--version"},
    {"su", "--version"}, {"mount", "--version"}, {"lsblk", "--version"}, {"lscpu", "--version"},
    {"free", "--version"}, {"ps", "--version"}, {"kill", "--version"}, {"dmesg", "--version"},
    {"systemctl", "--version"}, {"journalctl", "--version"}, {"pacman", "--version"},
    {"python", "--version"}, {"python3", "--version"}, {"perl", "--version"},
    {"bash", "--version"}, {"zsh", "--version"}, {"sh", "--version"},
    {"vim", "--version"}, {"nano", "--version"}, {"less", "--version"},
    {"ip", "-V"}, {"ss", "-V"}, {"ping", "-V"}, {"lsusb", "--version"}, {"lspci", "--version"},
    {"ollama", "--version"},
};
#define N_CATALOGO (sizeof CATALOGO / sizeof *CATALOGO)

static const char *const ENV_PROBE[] = {"PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C", "HOME=/nonexistent", 0};

void lia_executar(const char *caminho, const char *const *av, const char *const *env, Res *r, u32 limite_ms)
{
    memset(r, 0, sizeof *r);
    r->saida = -1;
    int po[2], pe[2], px[2];
    if (lia_pipe2(po, O_CLOEXEC) < 0 || lia_pipe2(pe, O_CLOEXEC) < 0 || lia_pipe2(px, O_CLOEXEC) < 0) {
        r->exec_errno = 24;
        return;
    }
    u64 t0 = agora_ms();
    long pid = lia_fork();
    if (pid == 0) {
        long nul = lia_openat(AT_FDCWD, "/dev/null", O_RDONLY, 0);
        if (nul > 0)
            lia_dup3((int)nul, 0, 0);
        lia_dup3(po[1], 1, 0);
        lia_dup3(pe[1], 2, 0);
        long rc = lia_execve(caminho, av, env);
        int e = (int)-rc;
        lia_write(px[1], &e, sizeof e);
        lia_exit(127);
    }
    lia_close(po[1]);
    lia_close(pe[1]);
    lia_close(px[1]);
    if (pid < 0) {
        r->exec_errno = (int)-pid;
        lia_close(po[0]);
        lia_close(pe[0]);
        lia_close(px[0]);
        return;
    }
    int fds[3] = {po[0], pe[0], px[0]};
    for (;;) {
        struct lia_pollfd p[3];
        int vivos = 0;
        for (int i = 0; i < 3; i++) {
            p[i].fd = fds[i];
            p[i].events = POLLIN;
            p[i].revents = 0;
            if (fds[i] >= 0)
                vivos++;
        }
        if (!vivos)
            break;
        u64 dec = agora_ms() - t0;
        if (dec >= limite_ms) {
            lia_kill(pid, 9);
            r->estourou = 1;
            break;
        }
        long n = lia_poll(p, 3, (int)(limite_ms - dec));
        if (n == -EINTR)
            continue;
        if (n < 0)
            break;
        for (int i = 0; i < 3; i++) {
            if (fds[i] < 0 || !(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char tmp[1024];
            long k = lia_read(fds[i], tmp, sizeof tmp);
            if (k == -EINTR)
                continue;
            if (k <= 0) {
                lia_close(fds[i]);
                fds[i] = -1;
                continue;
            }
            if (i == 0) {
                size_t c = PROBE_SAIDA - r->nout;
                if ((size_t)k < c)
                    c = (size_t)k;
                memcpy(r->out + r->nout, tmp, c);
                r->nout += c;
            } else if (i == 1) {
                size_t c = PROBE_SAIDA - r->nerr;
                if ((size_t)k < c)
                    c = (size_t)k;
                memcpy(r->err + r->nerr, tmp, c);
                r->nerr += c;
            } else if (k >= (long)sizeof(int)) {
                memcpy(&r->exec_errno, tmp, sizeof(int));
            }
        }
    }
    for (int i = 0; i < 3; i++)
        if (fds[i] >= 0)
            lia_close(fds[i]);
    int status = 0;
    lia_wait4(pid, &status, 0);
    r->ms = (u32)(agora_ms() - t0);
    if (!r->exec_errno)
        r->saida = (status & 0x7f) == 0 ? (status >> 8) & 0xff : 128 + (status & 0x7f);
}

static const char *primeira_linha(const char *s, size_t n, size_t *len)
{
    size_t i = 0;
    while (i < n && (s[i] == '\n' || s[i] == '\r' || s[i] == ' '))
        i++;
    size_t j = i;
    while (j < n && s[j] != '\n' && s[j] != '\r' && s[j])
        j++;
    if (j - i > 120)
        j = i + 120;
    *len = j - i;
    return s + i;
}

static void sondar(Ferr *f, const char *arg, int esperado)
{
    static Res r;
    char caminho[4200];
    lia_juntar(caminho, sizeof caminho, f->dir, "/", f->nome);
    const char *av[3] = {caminho, arg, 0};
    lia_executar(caminho, av, ENV_PROBE, &r, PROBE_LIMITE_MS);
    f->dur_ms = r.ms;
    f->saida = r.saida;
    f->versao = "";
    if (r.exec_errno) {
        f->estado = (r.exec_errno == 13 || r.exec_errno == 1) ? E_SEM_PERMISSAO : E_PROBE_FALHOU;
        return;
    }
    if (r.estourou) {
        f->estado = E_PRECISA_ENTRADA;
        return;
    }
    /* a versão sai em stdout ou stderr; vale a primeira linha que seja texto */
    size_t l = 0;
    const char *s = "";
    int texto = 0;
    for (int fonte = 0; fonte < 2 && !texto; fonte++) {
        s = fonte == 0 ? primeira_linha(r.out, r.nout, &l) : primeira_linha(r.err, r.nerr, &l);
        texto = l > 0;
        for (size_t i = 0; i < l; i++)
            if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
                texto = 0; /* saída binária não é uma versão */
    }
    f->versao = texto ? pool_dup(s, l) : "";
    f->estado = (r.saida == esperado && texto) ? E_PROBE_OK : E_PROBE_FALHOU;
}

/* ---------- persistência binária ---------- */

static void p16(Buf *b, u16 v) { b_put(b, &v, 2); }
static void p32(Buf *b, u32 v) { b_put(b, &v, 4); }
static void p64(Buf *b, u64 v) { b_put(b, &v, 8); }
static void pstr(Buf *b, const char *s)
{
    size_t n = lia_strlen(s) + 1;
    p16(b, (u16)n);
    b_put(b, s, n);
}

static int salvar_inventario(void)
{
    Buf b;
    b_init(&b, saidabuf, sizeof saidabuf, -1);
    b_put(&b, "LIA\0", 4);
    p32(&b, 1); /* versão do layout */
    p32(&b, 1); /* tipo: ferramentas */
    p32(&b, 0);
    p64(&b, nF);
    p64(&b, (u64)data_inventario);
    for (size_t i = 0; i < nF; i++) {
        Ferr *f = &F[i];
        p32(&b, f->mode);
        p32(&b, f->uid);
        p32(&b, f->gid);
        b_c(&b, (char)f->tipo);
        b_c(&b, (char)f->simlink);
        b_c(&b, (char)f->estado);
        b_c(&b, 0);
        p16(&b, f->maq);
        p16(&b, 0);
        p64(&b, f->size);
        p32(&b, (u32)f->saida);
        p32(&b, f->dur_ms);
        pstr(&b, f->nome);
        pstr(&b, f->dir);
        pstr(&b, f->alvo);
        pstr(&b, f->info);
        pstr(&b, f->versao);
    }
    if (b.err)
        return -1;
    p64(&b, fnv(b.p, b.n, FNV_INI));
    if (b.err)
        return -1;
    return gravar_arquivo(p_inv, b.p, b.n);
}

typedef struct {
    const unsigned char *p, *fim;
    int ruim;
} Cur;

static u64 rd(Cur *c, size_t n)
{
    u64 v = 0;
    if ((size_t)(c->fim - c->p) < n) {
        c->ruim = 1;
        return 0;
    }
    memcpy(&v, c->p, n);
    c->p += n;
    return v;
}

static const char *rstr(Cur *c)
{
    size_t n = (size_t)rd(c, 2);
    if (c->ruim || n == 0 || (size_t)(c->fim - c->p) < n || c->p[n - 1] != 0) {
        c->ruim = 1;
        return "";
    }
    const char *s = (const char *)c->p;
    c->p += n;
    return s;
}

static int carregar_inventario(void)
{
    long n = ler_arquivo(p_inv, arq, sizeof arq);
    if (n < 0) {
        b_s(&lia_err, "lia: inventário não encontrado; execute 'lia tools discover' primeiro\n");
        return -1;
    }
    if (n < 40 + 8 || memcmp(arq, "LIA\0", 4) != 0) {
        b_s(&lia_err, "lia: inventário inválido (assinatura)\n");
        return -1;
    }
    u64 soma;
    memcpy(&soma, arq + n - 8, 8);
    if (soma != fnv(arq, (size_t)n - 8, FNV_INI)) {
        b_s(&lia_err, "lia: inventário corrompido (checksum)\n");
        return -1;
    }
    Cur c = {(const unsigned char *)arq + 4, (const unsigned char *)arq + n - 8, 0};
    u32 versao = (u32)rd(&c, 4), tipo = (u32)rd(&c, 4);
    rd(&c, 4);
    u64 cont = rd(&c, 8);
    data_inventario = (i64)rd(&c, 8);
    if (versao != 1 || tipo != 1 || cont > MAX_FERR) {
        b_s(&lia_err, "lia: versão de inventário não suportada\n");
        return -1;
    }
    nF = 0;
    for (u64 i = 0; i < cont && !c.ruim; i++) {
        Ferr *f = &F[nF++];
        f->mode = (u32)rd(&c, 4);
        f->uid = (u32)rd(&c, 4);
        f->gid = (u32)rd(&c, 4);
        f->tipo = (u8)rd(&c, 1);
        f->simlink = (u8)rd(&c, 1);
        f->estado = (u8)rd(&c, 1);
        rd(&c, 1);
        f->maq = (u16)rd(&c, 2);
        rd(&c, 2);
        f->size = rd(&c, 8);
        f->saida = (i32)rd(&c, 4);
        f->dur_ms = (u32)rd(&c, 4);
        f->nome = rstr(&c);
        f->dir = rstr(&c);
        f->alvo = rstr(&c);
        f->info = rstr(&c);
        f->versao = rstr(&c);
        if (f->estado >= E_N || f->tipo > T_VAZIO)
            c.ruim = 1;
    }
    if (c.ruim || c.p != c.fim) {
        b_s(&lia_err, "lia: inventário malformado\n");
        return -1;
    }
    return 0;
}

/* ---------- relatório legível ---------- */

static void contar(size_t cont[E_N])
{
    memset(cont, 0, E_N * sizeof *cont);
    for (size_t i = 0; i < nF; i++)
        cont[F[i].estado]++;
}

static int escrever_md(void)
{
    Buf b;
    b_init(&b, saidabuf, sizeof saidabuf, -1);
    struct lia_utsname u;
    memset(&u, 0, sizeof u);
    lia_uname(&u);
    static char osr[8192];
    long n = ler_arquivo("/etc/os-release", osr, sizeof osr - 1);
    const char *distro = "desconhecida";
    char nomedistro[128] = "";
    if (n > 0) {
        osr[n] = 0;
        for (char *l = osr; *l;) {
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
                if (len > sizeof nomedistro - 1)
                    len = sizeof nomedistro - 1;
                memcpy(nomedistro, v, len);
                nomedistro[len] = 0;
                distro = nomedistro;
            }
            l = *fim ? fim + 1 : fim;
        }
    }
    size_t cont[E_N];
    contar(cont);

    b_s(&b, "# Ferramentas disponíveis nesta máquina\n\n");
    b_s(&b, "> Gerado por `lia tools`. Não edite: este arquivo é recriado a cada execução.\n\n");
    b_s(&b, "- Sistema: "), b_s(&b, distro), b_s(&b, "\n");
    b_s(&b, "- Kernel: "), b_s(&b, u.sysname), b_c(&b, ' '), b_s(&b, u.release), b_c(&b, ' '), b_s(&b, u.machine), b_s(&b, "\n");
    b_s(&b, "- Usuário: uid "), b_u(&b, (u64)lia_getuid()), b_s(&b, ", euid "), b_u(&b, (u64)lia_geteuid());
    b_s(&b, ", gid "), b_u(&b, (u64)lia_getgid()), b_s(&b, "\n");
    b_s(&b, "- Inventário de: "), b_data_utc(&b, data_inventario), b_s(&b, "\n");
    b_s(&b, "- Total de ferramentas: "), b_u(&b, nF), b_s(&b, "\n\n");

    b_s(&b, "## Estados\n\n| Estado | Quantidade |\n|---|---|\n");
    for (int e = 0; e < E_N; e++) {
        if (!cont[e])
            continue;
        b_s(&b, "| "), b_s(&b, NOME_ESTADO[e]), b_s(&b, " | "), b_u(&b, cont[e]), b_s(&b, " |\n");
    }

    b_s(&b, "\n## Probes executados\n\n| Ferramenta | Estado | Saída | ms | Versão |\n|---|---|---|---|---|\n");
    for (size_t i = 0; i < nF; i++) {
        Ferr *f = &F[i];
        if (f->estado != E_PROBE_OK && f->estado != E_PROBE_FALHOU && f->estado != E_PRECISA_ENTRADA)
            continue;
        b_s(&b, "| `"), b_s(&b, f->nome), b_s(&b, "` | "), b_s(&b, NOME_ESTADO[f->estado]), b_s(&b, " | ");
        b_i(&b, f->saida), b_s(&b, " | "), b_u(&b, f->dur_ms), b_s(&b, " | ");
        for (const char *v = f->versao; *v; v++) {
            if (*v == '|')
                b_c(&b, '\\');
            b_c(&b, *v);
        }
        b_s(&b, " |\n");
    }

    b_s(&b, "\n## Executáveis por diretório\n");
    const char *vistos[MAX_DIRS];
    size_t nv = 0;
    for (size_t i = 0; i < nF; i++) {
        size_t k;
        for (k = 0; k < nv; k++)
            if (!lia_strcmp(vistos[k], F[i].dir))
                break;
        if (k < nv || nv == MAX_DIRS)
            continue;
        vistos[nv++] = F[i].dir;
        size_t total = 0;
        for (size_t j = 0; j < nF; j++)
            if (!lia_strcmp(F[j].dir, F[i].dir))
                total++;
        b_s(&b, "\n### `"), b_s(&b, F[i].dir), b_s(&b, "` ("), b_u(&b, total), b_s(&b, ")\n\n");
        size_t col = 0;
        for (size_t j = 0; j < nF; j++) {
            if (lia_strcmp(F[j].dir, F[i].dir))
                continue;
            size_t l = lia_strlen(F[j].nome);
            if (col + l + 1 > 100) {
                b_c(&b, '\n');
                col = 0;
            }
            b_s(&b, F[j].nome), b_c(&b, ' ');
            col += l + 1;
        }
        b_c(&b, '\n');
    }
    if (b.err)
        return -1;
    return gravar_arquivo(p_md, b.p, b.n);
}

/* ---------- comandos ---------- */

static void resumo(void)
{
    size_t cont[E_N];
    contar(cont);
    b_u(&lia_out, nF), b_s(&lia_out, " ferramentas:");
    for (int e = 0; e < E_N; e++)
        if (cont[e])
            b_c(&lia_out, ' '), b_s(&lia_out, NOME_ESTADO[e]), b_c(&lia_out, '='), b_u(&lia_out, cont[e]);
    b_c(&lia_out, '\n');
}

static int salvar_tudo(void)
{
    if (salvar_inventario() < 0) {
        b_s(&lia_err, "lia: falha ao gravar o inventário binário\n");
        return 1;
    }
    if (escrever_md() < 0) {
        b_s(&lia_err, "lia: falha ao gravar o relatório legível\n");
        return 1;
    }
    b_s(&lia_out, "inventário: "), b_s(&lia_out, p_inv), b_c(&lia_out, '\n');
    b_s(&lia_out, "relatório:  "), b_s(&lia_out, p_md), b_c(&lia_out, '\n');
    return 0;
}

static int discover(void)
{
    struct lia_timespec ts;
    lia_clock(0, &ts);
    data_inventario = ts.s;
    varrer_tudo();
    resumo();
    b_s(&lia_out, "diretórios varridos: "), b_u(&lia_out, ndirs);
    b_s(&lia_out, ", sombreados por PATH: "), b_u(&lia_out, sombreadas);
    b_s(&lia_out, ", links quebrados: "), b_u(&lia_out, quebrados);
    b_s(&lia_out, ", sem bit de execução (ignorados): "), b_u(&lia_out, nao_exec), b_c(&lia_out, '\n');
    if (truncado)
        b_s(&lia_err, "aviso: limite de capacidade atingido; inventário parcial\n");
    return salvar_tudo();
}

static int probe(int argc, char **argv)
{
    int seguro = 0;
    for (int i = 0; i < argc; i++) {
        if (!lia_strcmp(argv[i], "--safe")) {
            seguro = 1;
        } else {
            b_s(&lia_err, "lia tools probe: opção desconhecida: "), b_s(&lia_err, argv[i]), b_c(&lia_err, '\n');
            return 2;
        }
    }
    if (!seguro) {
        b_s(&lia_err, "lia tools probe: use --safe (só testes conhecidos e não destrutivos)\n");
        return 2;
    }
    if (carregar_inventario() < 0)
        return 1;
    struct lia_timespec ts;
    lia_clock(0, &ts);
    data_inventario = ts.s;
    for (size_t i = 0; i < nF; i++) {
        Ferr *f = &F[i];
        if (f->estado == E_SEM_PERMISSAO || f->estado == E_DESCOBERTA)
            continue;
        const char *arg = 0;
        int esperado = !lia_strcmp(f->nome, "false"); /* false sai com 1 por definição */
        for (size_t k = 0; k < N_CATALOGO; k++)
            if (!lia_strcmp(CATALOGO[k].nome, f->nome)) {
                arg = CATALOGO[k].arg;
                break;
            }
        f->versao = "";
        f->saida = -1;
        f->dur_ms = 0;
        if (!arg) {
            f->estado = E_SEM_PROBE;
            continue;
        }
        sondar(f, arg, esperado);
        b_pad(&lia_out, NOME_ESTADO[f->estado], 18);
        b_pad(&lia_out, f->nome, 14);
        b_u(&lia_out, f->dur_ms), b_s(&lia_out, "ms  ");
        b_s(&lia_out, f->versao), b_c(&lia_out, '\n');
    }
    resumo();
    if (truncado)
        b_s(&lia_err, "aviso: memória de texto esgotada; versões podem estar vazias\n");
    return salvar_tudo();
}

static int listar(int argc, char **argv)
{
    int json = 0, filtro = -1;
    for (int i = 0; i < argc; i++) {
        if (!lia_strcmp(argv[i], "--json")) {
            json = 1;
        } else if (!lia_strcmp(argv[i], "--estado") && i + 1 < argc) {
            const char *e = argv[++i];
            for (int k = 0; k < E_N; k++)
                if (!lia_strcmp(NOME_ESTADO[k], e))
                    filtro = k;
            if (filtro < 0) {
                b_s(&lia_err, "lia tools list: estado desconhecido: "), b_s(&lia_err, e), b_c(&lia_err, '\n');
                return 2;
            }
        } else {
            b_s(&lia_err, "lia tools list: opção desconhecida: "), b_s(&lia_err, argv[i]), b_c(&lia_err, '\n');
            return 2;
        }
    }
    if (carregar_inventario() < 0)
        return 1;
    int primeiro = 1;
    if (json)
        b_s(&lia_out, "[");
    for (size_t i = 0; i < nF; i++) {
        Ferr *f = &F[i];
        if (filtro >= 0 && f->estado != filtro)
            continue;
        if (json) {
            b_s(&lia_out, primeiro ? "\n  " : ",\n  ");
            b_s(&lia_out, "{\"nome\":"), b_json(&lia_out, f->nome);
            b_s(&lia_out, ",\"dir\":"), b_json(&lia_out, f->dir);
            b_s(&lia_out, ",\"estado\":"), b_json(&lia_out, NOME_ESTADO[f->estado]);
            b_s(&lia_out, ",\"tipo\":"), b_json(&lia_out, NOME_TIPO[f->tipo]);
            b_s(&lia_out, ",\"simlink\":"), b_s(&lia_out, f->simlink ? "true" : "false");
            b_s(&lia_out, ",\"alvo\":"), b_json(&lia_out, f->alvo);
            b_s(&lia_out, ",\"modo\":\""), b_oct(&lia_out, f->mode & 07777);
            b_s(&lia_out, "\",\"uid\":"), b_u(&lia_out, f->uid);
            b_s(&lia_out, ",\"gid\":"), b_u(&lia_out, f->gid);
            b_s(&lia_out, ",\"tamanho\":"), b_u(&lia_out, f->size);
            b_s(&lia_out, ",\"saida\":"), b_i(&lia_out, f->saida);
            b_s(&lia_out, ",\"ms\":"), b_u(&lia_out, f->dur_ms);
            b_s(&lia_out, ",\"versao\":"), b_json(&lia_out, f->versao);
            b_s(&lia_out, "}");
            primeiro = 0;
        } else {
            b_pad(&lia_out, NOME_ESTADO[f->estado], 18);
            b_pad(&lia_out, f->nome, 28);
            b_pad(&lia_out, NOME_TIPO[f->tipo], 8);
            b_s(&lia_out, f->dir), b_c(&lia_out, '/'), b_s(&lia_out, f->nome);
            if (f->versao[0])
                b_s(&lia_out, "  # "), b_s(&lia_out, f->versao);
            b_c(&lia_out, '\n');
        }
    }
    if (json)
        b_s(&lia_out, primeiro ? "]\n" : "\n]\n");
    return 0;
}

int cmd_tools(int argc, char **argv)
{
    if (argc < 1) {
        b_s(&lia_err, "uso: lia tools <discover|list|probe>\n");
        return 2;
    }
    const char *sub = argv[0];
    if (lia_preparar() < 0)
        return 1;
    if (!lia_strcmp(sub, "discover")) {
        if (argc > 1) {
            b_s(&lia_err, "lia tools discover: não aceita argumentos\n");
            return 2;
        }
        return discover();
    }
    if (!lia_strcmp(sub, "list"))
        return listar(argc - 1, argv + 1);
    if (!lia_strcmp(sub, "probe"))
        return probe(argc - 1, argv + 1);
    b_s(&lia_err, "lia tools: subcomando desconhecido: "), b_s(&lia_err, sub), b_c(&lia_err, '\n');
    return 2;
}
