/* Testes unitários diretos de ollama/cliente_ollama.c e
 * sessiond/modelo_ollama.c — correções da auditoria pós-commit e366f2b:
 * validação centralizada de configuração (host/porta/timeout/modelo) e
 * o caminho de "status" (disponivel()) nunca conectando fora de
 * 127.0.0.1/localhost. Cada "modo" é um processo separado porque
 * sessiond/modelo_ollama.c guarda a config em um singleton estático
 * (g_cfg), lido uma única vez por processo a partir do ambiente — ver
 * testes/integration/teste_cliente_ollama.sh.
 */
#include "../../sessiond/modelo.h"
#include "../../ollama/cliente_ollama.h"
#include <glib.h>
#include <stdio.h>
#include <string.h>

static int falhas = 0;

static void checar(gboolean cond, const char *descricao)
{
    if (cond) {
        printf("ok   %s\n", descricao);
    } else {
        printf("FALHA %s\n", descricao);
        falhas++;
    }
}

static int modo_config_validar(void)
{
    OllamaConfig *cfg;

    cfg = ollama_config_novo("127.0.0.1", 11434, "llama3");
    char *erro = ollama_config_validar(cfg);
    checar(erro == NULL, "config válida (127.0.0.1:11434, llama3) -> aceita");
    g_free(erro);
    ollama_config_liberar(cfg);

    cfg = ollama_config_novo("localhost", 11434, "llama3");
    erro = ollama_config_validar(cfg);
    checar(erro == NULL, "config válida (localhost:11434) -> aceita (resolve e confirma loopback)");
    g_free(erro);
    ollama_config_liberar(cfg);

    cfg = ollama_config_novo("evil.example.com", 11434, "llama3");
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "host não-loopback ('evil.example.com') -> rejeitado");
    g_free(erro);
    ollama_config_liberar(cfg);

    cfg = ollama_config_novo("127.0.0.1", 11434, "llama3");
    cfg->porta = 0;
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "porta 0 -> rejeitada");
    g_free(erro);
    cfg->porta = -1;
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "porta negativa -> rejeitada");
    g_free(erro);
    cfg->porta = 65536;
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "porta 65536 (acima do limite) -> rejeitada, nunca truncada para guint16 silenciosamente");
    g_free(erro);
    cfg->porta = 70000;
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "porta 70000 -> rejeitada");
    g_free(erro);
    cfg->porta = 65535;
    erro = ollama_config_validar(cfg);
    checar(erro == NULL, "porta 65535 (limite máximo válido) -> aceita");
    g_free(erro);
    cfg->porta = 1;
    erro = ollama_config_validar(cfg);
    checar(erro == NULL, "porta 1 (limite mínimo válido) -> aceita");
    g_free(erro);
    cfg->porta = 11434;

    cfg->timeout_ms = 0;
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "timeout_ms 0 -> rejeitado (nunca chega a g_timeout_add com 0)");
    g_free(erro);
    cfg->timeout_ms = -5;
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "timeout_ms negativo -> rejeitado (nunca reinterpretado como guint enorme)");
    g_free(erro);
    cfg->timeout_ms = 700000;
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "timeout_ms acima do limite (700000) -> rejeitado");
    g_free(erro);
    cfg->timeout_ms = 15000;
    erro = ollama_config_validar(cfg);
    checar(erro == NULL, "timeout_ms 15000 (padrão) -> aceito");
    g_free(erro);

    g_free(cfg->modelo);
    cfg->modelo = g_strdup("");
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "nome do modelo vazio -> rejeitado");
    g_free(erro);

    g_free(cfg->modelo);
    cfg->modelo = g_strnfill(300, 'x');
    erro = ollama_config_validar(cfg);
    checar(erro != NULL, "nome do modelo acima do limite (300 bytes) -> rejeitado");
    g_free(erro);

    ollama_config_liberar(cfg);

    checar(ollama_host_e_loopback("127.0.0.1"), "ollama_host_e_loopback('127.0.0.1') -> true");
    checar(ollama_host_e_loopback("localhost"), "ollama_host_e_loopback('localhost') -> true");
    checar(!ollama_host_e_loopback("evil.example.com"), "ollama_host_e_loopback('evil.example.com') -> false");
    checar(!ollama_host_e_loopback(NULL), "ollama_host_e_loopback(NULL) -> false");

    return falhas;
}

/* Lê a config inteiramente do ambiente (LIA_OLLAMA_HOST/PORTA), como
 * sessiond/modelo_ollama.c faz — chama disponivel() e confirma que, para
 * um host inválido, o detalhe fala de configuração/host, nunca de
 * "indisponível"/"connection refused" (o que provaria que uma tentativa
 * de conexão de rede chegou a ser feita). */
static int modo_status_host_invalido(void)
{
    const ModeloBackend *m = modelo_ollama_obter();
    char *detalhe = NULL;
    gboolean ok = m->disponivel(m, &detalhe);
    printf("disponivel=%d detalhe=%s\n", ok, detalhe ? detalhe : "(null)");
    checar(!ok, "status com host inválido -> disponivel() retorna FALSE");
    gboolean fala_de_config = detalhe && (strstr(detalhe, "inválid") || strstr(detalhe, "invalid"));
    gboolean fala_de_rede = detalhe && (strstr(detalhe, "indisponível") || strstr(detalhe, "refused") || strstr(detalhe, "Connection"));
    checar(fala_de_config, "detalhe menciona configuração/host inválido (validado antes de qualquer E/S)");
    checar(!fala_de_rede, "detalhe NUNCA fala de conexão recusada/indisponível (prova que nenhuma tentativa de rede foi feita)");
    g_free(detalhe);
    return falhas;
}

static int modo_status_host_valido_sem_servidor(void)
{
    /* host válido (127.0.0.1), mas nenhum servidor Ollama ouvindo na
     * porta configurada: aqui SIM esperamos uma tentativa de conexão
     * real (e ela falhando, porque não há nada ouvindo) — prova que a
     * validação não vira um bloqueio geral para hosts válidos. */
    const ModeloBackend *m = modelo_ollama_obter();
    char *detalhe = NULL;
    gboolean ok = m->disponivel(m, &detalhe);
    printf("disponivel=%d detalhe=%s\n", ok, detalhe ? detalhe : "(null)");
    checar(!ok, "status com host válido mas sem servidor -> disponivel() retorna FALSE");
    g_free(detalhe);
    return falhas;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "uso: %s <config_validar|status_host_invalido|status_host_valido_sem_servidor>\n", argv[0]);
        return 2;
    }
    int r;
    if (!strcmp(argv[1], "config_validar")) r = modo_config_validar();
    else if (!strcmp(argv[1], "status_host_invalido")) r = modo_status_host_invalido();
    else if (!strcmp(argv[1], "status_host_valido_sem_servidor")) r = modo_status_host_valido_sem_servidor();
    else { fprintf(stderr, "modo desconhecido: %s\n", argv[1]); return 2; }
    printf("%d falhas\n", r);
    return r > 0 ? 1 : 0;
}
