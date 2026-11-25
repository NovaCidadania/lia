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
    {
        char *f = llama_cpp_formatar_prompt(NULL, "oi");
        checar(!strcmp(f, "oi"), "formato padrão (cru): prompt sem modelo de conversa");
        g_free(f);
        f = llama_cpp_formatar_prompt("chatml", "oi");
        checar(!strcmp(f, "<|im_start|>user\noi<|im_end|>\n<|im_start|>assistant\n"), "formato chatml embrulha o prompt");
        g_free(f);
        f = llama_cpp_formatar_prompt("llama3", "oi");
        checar(g_str_has_prefix(f, "<|start_header_id|>user") && strstr(f, "oi<|eot_id|>"), "formato llama3 embrulha o prompt");
        g_free(f);
        f = llama_cpp_formatar_prompt("desconhecido", "oi");
        checar(!strcmp(f, "oi"), "formato desconhecido -> cru");
        g_free(f);
    }
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

/* fim <esperado>: pergunta pelo backend (porta do servidor falso no
 * ambiente) e confere ModeloResposta.fim — "limite_tokens" quando o
 * servidor diz que parou no limite, "completo" quando o modelo terminou */
static char *g_fim; static gint64 g_tokens; static gboolean g_feito;
static void ao_fim(gpointer ctx, const ModeloResposta *r)
{
    (void)ctx;
    g_fim = g_strdup(r->fim ? r->fim : "(nulo)");
    g_tokens = r->tokens_gerados;
    g_feito = TRUE;
}
static int modo_fim(const char *esperado, gint64 tokens_esperados)
{
    ModeloOpcoes op = { .esquema_json = NULL, .max_tokens = 7, .resumo = TRUE };
    modelo_llama_cpp_obter()->perguntar(modelo_llama_cpp_obter(), "p1", "conte", &op, ao_fim, NULL);
    GMainContext *ctx = g_main_context_default();
    gint64 limite = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!g_feito && g_get_monotonic_time() < limite) g_main_context_iteration(ctx, TRUE);
    char *d = g_strdup_printf("término da geração = %s (esperado %s), tokens %" G_GINT64_FORMAT, g_fim ? g_fim : "(sem resposta)", esperado, g_tokens);
    checar(g_fim && !strcmp(g_fim, esperado) && (tokens_esperados < 0 || g_tokens == tokens_esperados), d);
    g_free(d);
    return falhas;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "fim")) {
        int r = modo_fim(argv[2], argc >= 4 ? g_ascii_strtoll(argv[3], NULL, 10) : -1);
        printf("%d falhas\n", r);
        return r > 0 ? 1 : 0;
    }
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
