/* lia — ponto de entrada da CLI. Sem libc, sem shell. */
#include "cli.h"

char **lia_envp;
Buf lia_out, lia_err;
static char mem_out[65536], mem_err[4096];

const char *lia_getenv(const char *nome)
{
    size_t n = lia_strlen(nome);
    for (char **e = lia_envp; e && *e; e++)
        if (lia_strncmp(*e, nome, n) == 0 && (*e)[n] == '=')
            return *e + n + 1;
    return 0;
}

static void ajuda(void)
{
    b_s(&lia_out,
        "lia " LIA_VERSAO " — IA local e agêntica\n"
        "\n"
        "uso: lia [--help] [--version] <comando> [argumentos]\n"
        "\n"
        "comandos:\n"
        "  tools discover          inventaria os executáveis do sistema (PATH e diretórios convencionais)\n"
        "  tools list [--estado E] [--json]\n"
        "                          consulta o inventário salvo\n"
        "  tools probe --safe      executa só testes conhecidos e não destrutivos\n"
        "\n"
        "  shell [-c CMD]          abre o shell do usuário (ou roda CMD com semântica de shell)\n"
        "  sudo ARGS...            executa o sudo do sistema com ARGS, sem shell no meio\n"
        "  root                    shell de root via sudo (a pessoa autentica; a Lia não guarda senha)\n"
        "  root -- CMD ARGS...     executa CMD como root\n"
        "  root --status           diz se root está disponível (exit 0 ok, 10 pede senha, 11 negado)\n"
        "  root --auth             autentica o sudo (sudo -v) para as próximas ações\n"
        "\n"
        "  --dry-run em shell/sudo/root mostra o que seria executado. Toda execução é\n"
        "  registrada em ~/.lia/humano/auditoria/AAAA-MM-DD.log antes de rodar.\n"
        "\n"
        "O inventário é gravado em ~/.lia/maquina/inventario/ferramentas.lia (binário)\n"
        "e ~/.lia/humano/diagnosticos/ferramentas.md (legível).\n");
}

int lia_main(long argc, char **argv, char **envp)
{
    lia_envp = envp;
    b_init(&lia_out, mem_out, sizeof mem_out, 1);
    b_init(&lia_err, mem_err, sizeof mem_err, 2);

    int rc;
    if (argc < 2) {
        /* nunca bloqueia esperando entrada: sem argumentos, mostra a ajuda */
        ajuda();
        rc = 0;
    } else if (!lia_strcmp(argv[1], "--help") || !lia_strcmp(argv[1], "-h")) {
        ajuda();
        rc = 0;
    } else if (!lia_strcmp(argv[1], "--version") || !lia_strcmp(argv[1], "-V")) {
        b_s(&lia_out, "lia " LIA_VERSAO " (x86_64-linux, sem libc)\n");
        rc = 0;
    } else if (!lia_strcmp(argv[1], "tools")) {
        rc = cmd_tools((int)argc - 2, argv + 2);
    } else if (!lia_strcmp(argv[1], "shell")) {
        rc = cmd_shell((int)argc - 2, argv + 2);
    } else if (!lia_strcmp(argv[1], "sudo")) {
        rc = cmd_sudo((int)argc - 2, argv + 2);
    } else if (!lia_strcmp(argv[1], "root")) {
        rc = cmd_root((int)argc - 2, argv + 2);
    } else {
        b_s(&lia_err, "lia: comando desconhecido: ");
        b_s(&lia_err, argv[1]);
        b_s(&lia_err, "\nuse 'lia --help'\n");
        rc = 2;
    }
    b_flush(&lia_out);
    b_flush(&lia_err);
    return lia_out.err ? 1 : rc;
}
