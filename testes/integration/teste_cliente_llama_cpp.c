/* Testes unitários diretos de ollama/cliente_llama_cpp.c e
 * sessiond/modelo_llama_cpp.c — espelha
 * testes/integration/teste_cliente_ollama.c. Cada "modo" é um processo
 * separado porque sessiond/modelo_llama_cpp.c guarda a config em um
 * singleton estático (g_cfg), lido uma única vez por processo a partir
 * do ambiente — ver testes/integration/teste_cliente_llama_cpp.sh.
 */
#include "../../sessiond/modelo.h"
#include "../../ollama/cliente_llama_cpp.h"
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
    LlamaCppConfig *cfg;

    cfg = llama_cpp_config_novo("127.0.0.1", 8080);
    char *erro = llama_cpp_config_validar(cfg);
    checar(erro == NULL, "config válida (127.0.0.1:8080) -> aceita");
    g_free(erro);
    llama_cpp_config_liberar(cfg);

    cfg = llama_cpp_config_novo("localhost", 8080);
    erro = llama_cpp_config_validar(cfg);
    checar(erro == NULL, "config válida (localhost:8080) -> aceita (resolve e confirma loopback)");
    g_free(erro);
    llama_cpp_config_liberar(cfg);

    cfg = llama_cpp_config_novo("evil.example.com", 8080);
    erro = llama_cpp_config_validar(cfg);
    checar(erro != NULL, "host não-loopback ('evil.example.com') -> rejeitado");
    g_free(erro);
    llama_cpp_config_liberar(cfg);

    cfg = llama_cpp_config_novo("127.0.0.1", 8080);
    cfg->porta = 0;
    erro = llama_cpp_config_validar(cfg);
    checar(erro != NULL, "porta 0 -> rejeitada");
    g_free(erro);
    cfg->porta = -1;
    erro = llama_cpp_config_validar(cfg);
    checar(erro != NULL, "porta negativa -> rejeitada");
    g_free(erro);
    cfg->porta = 65536;
    erro = llama_cpp_config_validar(cfg);
    checar(erro != NULL, "porta 65536 (acima do limite) -> rejeitada");
    g_free(erro);
    cfg->porta = 65535;
    erro = llama_cpp_config_validar(cfg);
    checar(erro == NULL, "porta 65535 (limite máximo válido) -> aceita");
    g_free(erro);
    cfg->porta = 1;
    erro = llama_cpp_config_validar(cfg);
    checar(erro == NULL, "porta 1 (limite mínimo válido) -> aceita");
    g_free(erro);
    cfg->porta = 8080;

    cfg->timeout_ms = 0;
    erro = llama_cpp_config_validar(cfg);
    checar(erro != NULL, "timeout_ms 0 -> rejeitado");
    g_free(erro);
    cfg->timeout_ms = -5;
    erro = llama_cpp_config_validar(cfg);
    checar(erro != NULL, "timeout_ms negativo -> rejeitado");
    g_free(erro);
    cfg->timeout_ms = 700000;
    erro = llama_cpp_config_validar(cfg);
    checar(erro != NULL, "timeout_ms acima do limite (700000) -> rejeitado");
    g_free(erro);
    cfg->timeout_ms = 30000;
    erro = llama_cpp_config_validar(cfg);
    checar(erro == NULL, "timeout_ms 30000 (padrão) -> aceito");
    g_free(erro);

    llama_cpp_config_liberar(cfg);

    checar(llama_cpp_host_e_loopback("127.0.0.1"), "llama_cpp_host_e_loopback('127.0.0.1') -> true");
    checar(llama_cpp_host_e_loopback("localhost"), "llama_cpp_host_e_loopback('localhost') -> true");
    checar(!llama_cpp_host_e_loopback("evil.example.com"), "llama_cpp_host_e_loopback('evil.example.com') -> false");
    checar(!llama_cpp_host_e_loopback(NULL), "llama_cpp_host_e_loopback(NULL) -> false");

    return falhas;
}

static int modo_status_host_invalido(void)
{
    const ModeloBackend *m = modelo_llama_cpp_obter();
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
    const ModeloBackend *m = modelo_llama_cpp_obter();
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
