/*
 * lia shell | lia sudo | lia root — shell e privilégios de administrador.
 *
 * Objetivo inicial da Lia: rodar shell e sudo e obter root o quanto antes no
 * dispositivo. Root é obtido pelo caminho legítimo do sistema: o `sudo` real,
 * com a autenticação da própria pessoa. A Lia nunca guarda senha e nunca
 * explora vulnerabilidades; o cache de credenciais é do próprio sudo.
 *
 * Tudo é executado com execve direto (sem shell implícito) e registrado em
 * ~/.lia/humano/auditoria/AAAA-MM-DD.log antes de rodar.
 */
#include "cli.h"

#define MAX_ARGS 256

static const char *const ENV_C[] = {"PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C", 0};

/* ---------- utilitários ---------- */

static int executavel(const char *c)
{
    struct lia_stat st;
    return lia_fstatat(AT_FDCWD, c, &st, 0) == 0 && (st.mode & S_IFMT) == S_IFREG &&
           lia_faccessat2(AT_FDCWD, c, 1, AT_EACCESS) == 0;
}

/* Procura `nome` no PATH (na ordem) e nos diretórios convencionais. */
static const char *resolver(const char *nome, char *dst, size_t cap)
{
    const char *path = lia_getenv("PATH");
    while (path && *path) {
        size_t n = 0;
        while (path[n] && path[n] != ':')
            n++;
        if (n > 0 && n + lia_strlen(nome) + 2 <= cap) {
            memcpy(dst, path, n);
            dst[n] = '/';
            memcpy(dst + n + 1, nome, lia_strlen(nome) + 1);
            if (executavel(dst))
                return dst;
        }
        path += n;
        if (*path == ':')
            path++;
    }
    static const char *const conv[] = {"/usr/local/bin", "/usr/bin", "/bin", "/usr/local/sbin", "/usr/sbin", "/sbin"};
    for (size_t i = 0; i < sizeof conv / sizeof *conv; i++)
        if (lia_juntar(dst, cap, conv[i], "/", nome) == 0 && executavel(dst))
            return dst;
    return 0;
}

static const char *achar_shell(char *dst, size_t cap)
{
    const char *s = lia_getenv("SHELL");
    if (s && s[0] == '/' && executavel(s) && lia_strlen(s) < cap) {
        memcpy(dst, s, lia_strlen(s) + 1);
        return dst;
    }
    if (resolver("bash", dst, cap) || resolver("sh", dst, cap))
        return dst;
    return executavel("/bin/sh") ? "/bin/sh" : 0;
}

static int contem(const char *h, const char *n)
{
    size_t ln = lia_strlen(n);
    for (; *h; h++)
        if (!lia_strncmp(h, n, ln))
            return 1;
    return 0;
}

static void descrever(Buf *b, const char *caminho, const char *const *argv)
{
    b_json(b, caminho);
    b_s(b, " [");
    for (int i = 0; argv[i]; i++) {
        if (i)
            b_c(b, ',');
        b_json(b, argv[i]);
    }
    b_c(b, ']');
}

/* Registro append-only: acontece ANTES da execução. */
static void auditar(const char *acao, const char *caminho, const char *const *argv)
{
    if (lia_preparar() < 0)
        return;
    struct lia_timespec ts;
    lia_clock(0, &ts);
    char dm[64], mem[8192], arquivo[1200];
    Buf d, l;
    b_init(&d, dm, sizeof dm - 1, -1);
    b_data_utc(&d, ts.s);
    dm[10] = 0;
    b_init(&l, mem, sizeof mem, -1);
    b_data_utc(&l, ts.s);
    b_s(&l, " uid="), b_u(&l, (u64)lia_getuid());
    b_s(&l, " euid="), b_u(&l, (u64)lia_geteuid());
    b_s(&l, " acao="), b_s(&l, acao);
    b_s(&l, " exec="), descrever(&l, caminho, argv);
    b_c(&l, '\n');
    if (l.err || lia_juntar(arquivo, sizeof arquivo, lia_home(), "/.lia/humano/auditoria/", dm) < 0 ||
        lia_juntar(arquivo + lia_strlen(arquivo), 16, ".log", "", "") < 0) {
        b_s(&lia_err, "lia: aviso: não foi possível montar o registro de auditoria\n");
        return;
    }
    long fd = lia_openat(AT_FDCWD, arquivo, O_WRONLY | O_CREAT | 02000 /* O_APPEND */ | O_CLOEXEC, 0600);
    if (fd < 0 || lia_escrever_tudo((int)fd, l.p, l.n) < 0)
        b_s(&lia_err, "lia: aviso: falha ao gravar a auditoria\n");
    if (fd >= 0)
        lia_close((int)fd);
}

/* Substitui o processo (stdio, TTY e sinais ficam com o programa executado). */
static int substituir(const char *acao, const char *caminho, const char *const *argv, int simular)
{
    if (simular) {
        b_s(&lia_out, "dry-run: "), descrever(&lia_out, caminho, argv), b_c(&lia_out, '\n');
        return 0;
    }
    auditar(acao, caminho, argv);
    b_flush(&lia_out);
    b_flush(&lia_err);
    long rc = lia_execve(caminho, argv, (const char *const *)lia_envp);
    b_s(&lia_err, "lia: execve falhou: "), b_s(&lia_err, caminho);
    b_s(&lia_err, " (errno "), b_u(&lia_err, (u64)-rc), b_s(&lia_err, ")\n");
    return 126;
}

/* Roda e espera, com stdio herdado (precisa do TTY para a senha do sudo). */
static int rodar_e_esperar(const char *acao, const char *caminho, const char *const *argv)
{
    auditar(acao, caminho, argv);
    b_flush(&lia_out);
    b_flush(&lia_err);
    long pid = lia_fork();
    if (pid == 0) {
        lia_execve(caminho, argv, (const char *const *)lia_envp);
        lia_exit(127);
    }
    if (pid < 0)
        return 126;
    int status = 0;
    while (lia_wait4(pid, &status, 0) == -EINTR)
        ;
    return (status & 0x7f) == 0 ? (status >> 8) & 0xff : 128 + (status & 0x7f);
}

static int sem_sudo(void)
{
    b_s(&lia_err, "lia: sudo não encontrado neste sistema (instale o pacote 'sudo')\n");
    return 127;
}

/* ---------- lia shell ---------- */

int cmd_shell(int argc, char **argv)
{
    int simular = 0;
    const char *comando = 0;
    for (int i = 0; i < argc; i++) {
        if (!lia_strcmp(argv[i], "--dry-run")) {
            simular = 1;
        } else if (!lia_strcmp(argv[i], "-c") && i + 1 < argc) {
            comando = argv[++i];
        } else {
            b_s(&lia_err, "uso: lia shell [--dry-run] [-c COMANDO]\n");
            return 2;
        }
    }
    char sh[1024];
    const char *caminho = achar_shell(sh, sizeof sh);
    if (!caminho) {
        b_s(&lia_err, "lia: nenhum shell encontrado\n");
        return 127;
    }
    const char *av[4] = {caminho, 0, 0, 0};
    if (comando) {
        av[1] = "-c";
        av[2] = comando;
    }
    return substituir("shell", caminho, av, simular);
}

/* ---------- lia sudo ---------- */

int cmd_sudo(int argc, char **argv)
{
    int simular = 0;
    if (argc > 0 && !lia_strcmp(argv[0], "--dry-run")) {
        simular = 1;
        argc--;
        argv++;
    }
    if (argc > MAX_ARGS - 2) {
        b_s(&lia_err, "lia sudo: argumentos demais\n");
        return 2;
    }
    char sp[1024];
    const char *sudo = resolver("sudo", sp, sizeof sp);
    if (!sudo)
        return sem_sudo();
    const char *av[MAX_ARGS];
    av[0] = "sudo";
    for (int i = 0; i < argc; i++)
        av[i + 1] = argv[i];
    av[argc + 1] = 0;
    return substituir("sudo", sudo, av, simular);
}

/* ---------- lia root ---------- */

static int status_root(void)
{
    u64 uid = (u64)lia_getuid(), euid = (u64)lia_geteuid();
    b_s(&lia_out, "uid="), b_u(&lia_out, uid), b_s(&lia_out, " euid="), b_u(&lia_out, euid), b_c(&lia_out, '\n');
    if (euid == 0) {
        b_s(&lia_out, "ROOT: a Lia já está executando como root\n");
        return 0;
    }
    char sp[1024];
    const char *sudo = resolver("sudo", sp, sizeof sp);
    if (!sudo) {
        b_s(&lia_out, "SEM_SUDO: sudo não está instalado\n");
        return 127;
    }
    static Res r;
    const char *av[] = {"sudo", "-n", "true", 0};
    lia_executar(sudo, av, ENV_C, &r, 5000);
    r.err[PROBE_SAIDA - 1] = 0;
    if (r.exec_errno) {
        b_s(&lia_out, "INDETERMINADO: não foi possível executar o sudo\n");
        return 12;
    }
    if (r.saida == 0) {
        b_s(&lia_out, "ROOT_DISPONIVEL: sudo funciona sem pedir senha (credencial em cache ou NOPASSWD)\n");
        return 0;
    }
    if (contem(r.err, "password is required") || contem(r.err, "terminal is required")) {
        b_s(&lia_out, "ROOT_REQUER_SENHA: o usuário pode usar sudo; rode 'lia root --auth' para autenticar\n");
        return 10;
    }
    if (contem(r.err, "not in the sudoers") || contem(r.err, "may not run sudo") || contem(r.err, "is not allowed")) {
        b_s(&lia_out, "ROOT_NEGADO: este usuário não tem permissão de sudo (precisa estar no sudoers/grupo wheel)\n");
        return 11;
    }
    b_s(&lia_out, "INDETERMINADO: ");
    size_t n = 0;
    while (r.err[n] && r.err[n] != '\n' && n < 160)
        n++;
    b_put(&lia_out, r.err, n);
    b_c(&lia_out, '\n');
    return 12;
}

int cmd_root(int argc, char **argv)
{
    int simular = 0, auth = 0, status = 0, i = 0;
    for (; i < argc; i++) {
        if (!lia_strcmp(argv[i], "--dry-run"))
            simular = 1;
        else if (!lia_strcmp(argv[i], "--auth"))
            auth = 1;
        else if (!lia_strcmp(argv[i], "--status"))
            status = 1;
        else if (!lia_strcmp(argv[i], "--")) {
            i++;
            break;
        } else if (argv[i][0] == '-') {
            b_s(&lia_err, "uso: lia root [--status | --auth | --dry-run] [-- COMANDO [ARGS...]]\n");
            return 2;
        } else
            break;
    }
    int ncmd = argc - i;
    char **cmd = argv + i;
    if (ncmd > MAX_ARGS - 4) {
        b_s(&lia_err, "lia root: argumentos demais\n");
        return 2;
    }
    if (status)
        return status_root();

    char sp[1024], shp[1024];
    int ja_root = lia_geteuid() == 0;
    const char *sudo = 0;
    if (!ja_root && !(sudo = resolver("sudo", sp, sizeof sp)))
        return sem_sudo();

    if (auth) {
        if (ja_root) {
            b_s(&lia_out, "já é root; nada a autenticar\n");
            return 0;
        }
        const char *av[] = {"sudo", "-v", 0};
        if (simular) {
            b_s(&lia_out, "dry-run: "), descrever(&lia_out, sudo, av), b_c(&lia_out, '\n');
            return 0;
        }
        int rc = rodar_e_esperar("root-auth", sudo, av);
        if (rc == 0)
            b_s(&lia_out, "root autenticado: o sudo guarda a credencial por alguns minutos (a Lia não armazena a senha)\n");
        else
            b_s(&lia_err, "lia: autenticação do sudo falhou ou foi cancelada\n");
        return rc;
    }

    const char *av[MAX_ARGS];
    int n = 0;
    const char *alvo;
    if (ja_root) {
        if (ncmd == 0) {
            alvo = achar_shell(shp, sizeof shp);
            if (!alvo) {
                b_s(&lia_err, "lia: nenhum shell encontrado\n");
                return 127;
            }
            av[n++] = alvo;
        } else {
            alvo = cmd[0];
            for (int k = 0; k < ncmd; k++)
                av[n++] = cmd[k];
        }
    } else {
        alvo = sudo;
        av[n++] = "sudo";
        if (ncmd == 0) {
            av[n++] = "-i"; /* shell de login do root */
        } else {
            av[n++] = "--";
            for (int k = 0; k < ncmd; k++)
                av[n++] = cmd[k];
        }
    }
    av[n] = 0;
    return substituir("root", alvo, av, simular);
}
