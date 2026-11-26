/* Testes do backend experimental "lia_proprio" (sessiond/modelo_lia_proprio.c) com um CLI de mentira (script shell):
 * contrato de edições, falha clara sem checkpoint, sem recaída em outro modelo, resumo recusado, timeout, entrada ipsis litteris.
 * O modelo real (PyTorch) é testado em testes/modelo_proprio/teste_modelo.py. Uso: make teste-modelo-proprio-backend */
#include "../../sessiond/modelo.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

static int total, falhas;
#define checar(c, d) checar_((c) ? TRUE : FALSE, (d))
static void checar_(gboolean cond, const char *desc)
{
    total++;
    if (!cond) falhas++;
    printf("%s %s\n", cond ? "ok  " : "FALHA", desc);
}

typedef struct { GMainLoop *loop; int chamadas; gboolean sucesso; char *texto, *codigo, *msg, *fim; } Res;

static void ao_concluir(gpointer ctx, const ModeloResposta *r)
{
    Res *x = ctx;
    x->chamadas++;
    x->sucesso = r->sucesso;
    g_free(x->texto); g_free(x->codigo); g_free(x->msg); g_free(x->fim);
    x->texto = g_strdup(r->resposta_texto); x->codigo = g_strdup(r->erro_codigo); x->msg = g_strdup(r->erro_mensagem); x->fim = g_strdup(r->fim);
    g_main_loop_quit(x->loop);
}

static gboolean tempo_esgotado(gpointer l) { g_main_loop_quit(l); return G_SOURCE_REMOVE; }

static Res perguntar_esq(const char *prompt, gboolean resumo, const char *esquema)
{
    Res x = { .loop = g_main_loop_new(NULL, FALSE) };
    const ModeloBackend *b = modelo_lia_proprio_obter();
    ModeloOpcoes op = { .esquema_json = esquema, .resumo = resumo, .max_tokens = 100 };
    guint t = g_timeout_add_seconds(20, tempo_esgotado, x.loop);
    b->perguntar(b, "q1", prompt, &op, ao_concluir, &x);
    if (!x.chamadas) g_main_loop_run(x.loop);
    g_source_remove(t);
    g_main_loop_unref(x.loop);
    return x;
}

static Res perguntar(const char *prompt, gboolean resumo) { return perguntar_esq(prompt, resumo, NULL); }

static char *escrever_stub(const char *dir, const char *nome, const char *corpo)
{
    char *p = g_build_filename(dir, nome, NULL);
    g_file_set_contents(p, corpo, -1, NULL);
    chmod(p, 0755);
    return p;
}

int main(void)
{
    char *dir = g_dir_make_tmp("lia-proprio-XXXXXX", NULL);
    char *ck = g_build_filename(dir, "modelo.pt", NULL);
    g_file_set_contents(ck, "pesos de mentira", -1, NULL);
    char *marca = g_build_filename(dir, "stdin.txt", NULL);
    char *invocado = g_build_filename(dir, "invocado", NULL);

    /* ---- sem checkpoint: falha clara, o CLI nem é chamado ---- */
    char *s_ok_corpo = g_strdup_printf("#!/bin/sh\ntouch %s\ncat > %s\necho '{\"edicoes\":[{\"acao\":\"editar\",\"caminho\":\"m.py\",\"procurar\":\"a < b\",\"substituir\":\"a > b\"}],"
                                       "\"explicacao\":\"troca\",\"modelo\":{\"backend\":\"lia_proprio\"},\"motivo\":\"editar\"}'\n", invocado, marca);
    char *s_ok = escrever_stub(dir, "cli_ok.sh", s_ok_corpo);
    g_setenv("LIA_PROPRIO_CLI", s_ok, TRUE);
    g_unsetenv("LIA_PROPRIO_CHECKPOINT");
    Res r = perguntar("prompt", FALSE);
    checar(!r.sucesso && !g_strcmp0(r.codigo, "modelo_indisponivel") && r.msg && strstr(r.msg, "LIA_PROPRIO_CHECKPOINT"),
           "sem LIA_PROPRIO_CHECKPOINT: falha clara (modelo_indisponivel), sem usar outro modelo");
    checar(!g_file_test(invocado, G_FILE_TEST_EXISTS), "sem checkpoint o CLI nem foi executado");
    {
        const ModeloBackend *b = modelo_lia_proprio_obter();
        char *det = NULL;
        checar(!b->disponivel(b, &det) && det && strstr(det, "indisponível"), "disponivel() diz que está indisponível sem checkpoint");
        g_free(det);
        checar(!strcmp(b->nome_backend, "lia_proprio"), "nome do backend é lia_proprio");
        checar(modelo_lia_proprio_identidade() == NULL, "sem checkpoint não há identidade");
    }
    char *inex = g_build_filename(dir, "nao_existe.pt", NULL);
    g_setenv("LIA_PROPRIO_CHECKPOINT", inex, TRUE);
    r = perguntar("prompt", FALSE);
    checar(!r.sucesso && !g_strcmp0(r.codigo, "modelo_indisponivel") && r.msg && strstr(r.msg, "ausente"), "checkpoint inexistente: modelo_indisponivel");

    /* ---- caminho feliz: o prompt chega inteiro e o runtime recebe só o contrato de edições ---- */
    g_setenv("LIA_PROPRIO_CHECKPOINT", ck, TRUE);
    const char *prompt = "linha 1 \"aspas\"\n----- pedido -----\ncorrija ç ã\n";
    r = perguntar(prompt, FALSE);
    char *lido = NULL;
    g_file_get_contents(marca, &lido, NULL, NULL);
    checar(r.sucesso && r.texto && strstr(r.texto, "\"edicoes\"") && strstr(r.texto, "\"procurar\""), "contrato de edições entregue ao runtime");
    checar(r.texto && !strstr(r.texto, "\"modelo\"") && !strstr(r.texto, "conclu"), "o runtime recebe só edições+explicação (sem campo de conclusão)");
    checar(!g_strcmp0(r.fim, "completo"), "fim = completo");
    checar(!g_strcmp0(lido, prompt), "o prompt chegou ao CLI byte a byte pelo stdin");
    g_free(lido);
    {
        const ModeloBackend *b = modelo_lia_proprio_obter();
        char *det = NULL;
        checar(b->disponivel(b, &det) && det && strstr(det, "experimental"), "disponivel() com checkpoint e CLI");
        g_free(det);
        char *id = modelo_lia_proprio_identidade();
        char *esperado_sha = NULL;
        { gchar *dados; gsize n; g_file_get_contents(ck, &dados, &n, NULL); esperado_sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)dados, n); g_free(dados); }
        checar(id && strstr(id, "experimental") && strstr(id, "modelo.pt") && strstr(id, esperado_sha) == NULL && strncmp(strstr(id, "sha256 ") + 7, esperado_sha, 12) == 0,
               "identidade: experimental + arquivo + prefixo do sha256 do checkpoint");
        g_free(id); g_free(esperado_sha);
    }

    /* ---- resumo final: o modelo não resume nem conclui; o CLI não é chamado ---- */
    unlink(invocado);
    r = perguntar("resuma", TRUE);
    checar(!r.sucesso && !g_strcmp0(r.codigo, "fora_do_escopo") && !g_file_test(invocado, G_FILE_TEST_EXISTS), "resumo final é recusado (runtime usa o texto determinístico) e não chama o CLI");

    /* o pedido de edições do coding usa resumo=TRUE COM esquema: tem de chegar ao CLI */
    r = perguntar_esq("prompt de coding", TRUE, "{\"type\":\"object\"}");
    checar(r.sucesso && r.texto && strstr(r.texto, "\"edicoes\"") && g_file_test(invocado, G_FILE_TEST_EXISTS), "pedido de edições do coding (resumo=TRUE com esquema) é atendido");

    /* ---- CLI com checkpoint inválido (código 3), crash e lixo ---- */
    char *s3 = escrever_stub(dir, "cli_3.sh", "#!/bin/sh\ncat >/dev/null\necho 'lia-neural: checkpoint inválido: sha256' >&2\nexit 3\n");
    g_setenv("LIA_PROPRIO_CLI", s3, TRUE);
    r = perguntar("p", FALSE);
    checar(!r.sucesso && !g_strcmp0(r.codigo, "modelo_indisponivel") && r.msg && strstr(r.msg, "checkpoint inválido"), "checkpoint inválido no CLI (rc 3): modelo_indisponivel com a causa");
    char *s1 = escrever_stub(dir, "cli_1.sh", "#!/bin/sh\ncat >/dev/null\nexit 1\n");
    g_setenv("LIA_PROPRIO_CLI", s1, TRUE);
    r = perguntar("p", FALSE);
    checar(!r.sucesso && !g_strcmp0(r.codigo, "modelo_resposta_invalida"), "falha do CLI: modelo_resposta_invalida");
    char *sl = escrever_stub(dir, "cli_lixo.sh", "#!/bin/sh\ncat >/dev/null\necho 'isto não é json'\n");
    g_setenv("LIA_PROPRIO_CLI", sl, TRUE);
    r = perguntar("p", FALSE);
    checar(!r.sucesso && !g_strcmp0(r.codigo, "modelo_resposta_invalida") && !g_strcmp0(r.fim, "resposta_invalida"), "saída que não é JSON de edições: resposta_invalida (nada é inventado)");
    char *sj = escrever_stub(dir, "cli_sem.sh", "#!/bin/sh\ncat >/dev/null\necho '{\"explicacao\":\"x\"}'\n");
    g_setenv("LIA_PROPRIO_CLI", sj, TRUE);
    r = perguntar("p", FALSE);
    checar(!r.sucesso && !g_strcmp0(r.codigo, "modelo_resposta_invalida"), "JSON sem o campo edicoes: resposta_invalida");
    char *sv = escrever_stub(dir, "cli_vazio.sh", "#!/bin/sh\ncat >/dev/null\necho '{\"edicoes\":[],\"explicacao\":\"Qual arquivo?\",\"motivo\":\"esclarecer\"}'\n");
    g_setenv("LIA_PROPRIO_CLI", sv, TRUE);
    r = perguntar("p", FALSE);
    checar(r.sucesso && r.texto && strstr(r.texto, "\"edicoes\":[]") && strstr(r.texto, "Qual arquivo?"), "sem edição (pedir contexto): contrato vazio com a explicação do modelo");

    /* ---- executável do CLI ausente ---- */
    char *sem = g_build_filename(dir, "nao_existe.sh", NULL);
    g_setenv("LIA_PROPRIO_CLI", sem, TRUE);
    r = perguntar("p", FALSE);
    checar(!r.sucesso && !g_strcmp0(r.codigo, "modelo_indisponivel"), "CLI inexistente: modelo_indisponivel");

    /* ---- timeout: o processo é encerrado ---- */
    char *pid_arq = g_build_filename(dir, "pid", NULL);
    char *sd_corpo = g_strdup_printf("#!/bin/sh\necho $$ > %s\ncat >/dev/null\nexec sleep 30\n", pid_arq);
    char *sd = escrever_stub(dir, "cli_dorme.sh", sd_corpo);
    g_setenv("LIA_PROPRIO_CLI", sd, TRUE);
    g_setenv("LIA_PROPRIO_TIMEOUT_MS", "400", TRUE);
    r = perguntar("p", FALSE);
    checar(!r.sucesso && !g_strcmp0(r.codigo, "limite_excedido") && !g_strcmp0(r.fim, "timeout"), "timeout: limite_excedido");
    char *pid_s = NULL;
    g_file_get_contents(pid_arq, &pid_s, NULL, NULL);
    gboolean vivo = FALSE;
    if (pid_s) { char *prc = g_strdup_printf("/proc/%d", atoi(pid_s)); g_usleep(300000); vivo = g_file_test(prc, G_FILE_TEST_EXISTS); g_free(prc); }
    checar(pid_s && !vivo, "o processo do CLI foi encerrado no timeout");
    g_free(pid_s);
    g_unsetenv("LIA_PROPRIO_TIMEOUT_MS");

    printf("\n%d testes, %d falhas\n", total, falhas);
    return falhas ? 1 : 0;
}
