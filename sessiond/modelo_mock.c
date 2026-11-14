/* lia-sessiond — backend mock de modelo (sessiond/modelo.h): determinístico,
 * sem rede, sem LLM real — só para os testes do protocolo de chat. Nunca é
 * o padrão em produção (ver escolher_modelo em sessiond.c).
 *
 * Reconhece um marcador literal "MOCK_PROPOR:<ferramenta>" para forçar
 * deterministicamente uma proposta de ferramenta nos testes (inclusive
 * propor uma ferramenta inexistente ou proibida, de propósito —
 * docs/THREAT-MODEL.md §5.2, "o modelo pode sugerir qualquer coisa; só a
 * validação independente decide"). Fora disso, reage a algumas
 * palavras-chave simples do pedido da pessoa.
 *
 * Importante: todo esse reconhecimento só olha a seção "### PEDIDO DA
 * PESSOA USUÁRIA" do prompt (sessiond/prompt.c) — nunca as instruções de
 * sistema, o manifesto ou conteúdo de arquivo. Isto é deliberado e, na
 * prática, obrigatório: as próprias instruções de sistema hardcoded
 * contêm a palavra "sistema" ("instruções do sistema"), então procurar
 * "sistema"/"diagnost" no prompt inteiro faria QUALQUER pergunta,
 * inclusive uma sem relação nenhuma, disparar a sugestão de
 * sistema.diagnosticar — não é só por fidelidade à fronteira de
 * confiança, é a diferença entre o mock responder de propósito ou por
 * acidente. */
#include "modelo.h"
#include <string.h>

static gboolean contem_ci(const char *texto, const char *alvo)
{
    if (!texto || !alvo) return FALSE;
    char *t = g_utf8_strdown(texto, -1);
    char *a = g_utf8_strdown(alvo, -1);
    gboolean achou = strstr(t, a) != NULL;
    g_free(t); g_free(a);
    return achou;
}

static void mock_perguntar(const ModeloBackend *self, const char *pergunta_id,
                            const char *prompt_montado_completo, ModeloConcluidoCb concluido_cb, gpointer ctx)
{
    (void)self; (void)pergunta_id;
    ModeloResposta r = { 0 };
    r.sucesso = TRUE;

    const char *inicio_pedido = strstr(prompt_montado_completo, "### PEDIDO DA PESSOA USUÁRIA");
    if (inicio_pedido) {
        const char *quebra = strchr(inicio_pedido, '\n');
        inicio_pedido = quebra ? quebra + 1 : inicio_pedido;
    } else {
        inicio_pedido = prompt_montado_completo; /* falha segura: nunca trava, só perde a fronteira extra */
    }
    const char *fim_pedido = strstr(inicio_pedido, "### CONTEÚDO DE ARQUIVO");
    gsize tam_pedido = fim_pedido ? (gsize)(fim_pedido - inicio_pedido) : strlen(inicio_pedido);
    char *pedido_isolado = g_strndup(inicio_pedido, tam_pedido);
    const char *prompt_montado = pedido_isolado;

    const char *marcador = strstr(prompt_montado, "MOCK_PROPOR:");
    if (marcador) {
        marcador += strlen("MOCK_PROPOR:");
        const char *fim = marcador;
        while (*fim && *fim != '\n' && *fim != ' ' && *fim != '"') fim++;
        r.ferramenta_proposta = g_strndup(marcador, (gsize)(fim - marcador));
        r.parametros_json = g_strdup("{}");
        r.justificativa = g_strdup("proposta forçada por marcador de teste (MOCK_PROPOR)");
        r.resposta_texto = g_strdup_printf("(mock) sugiro usar '%s'.", r.ferramenta_proposta);
    } else if (strstr(prompt_montado, "MOCK_RESUMIR") || contem_ci(prompt_montado, "foi aprovada e executada")) {
        /* segunda chamada do fluxo (resumir um resultado já executado) —
         * nunca propõe outra ferramenta nesta etapa (evita encadeamento
         * automático de ações sem nova aprovação humana). */
        r.resposta_texto = g_strdup("(mock) resultado recebido e resumido.");
    } else if (contem_ci(prompt_montado, "processo") || contem_ci(prompt_montado, "cpu")) {
        r.ferramenta_proposta = g_strdup("sistema.processos_top_cpu");
        r.parametros_json = g_strdup("{\"limite\":10}");
        r.justificativa = g_strdup("o pedido menciona processos/CPU");
        r.resposta_texto = g_strdup("(mock) posso listar os processos que mais usam CPU.");
    } else if (contem_ci(prompt_montado, "diagnost") || contem_ci(prompt_montado, "sistema")) {
        r.ferramenta_proposta = g_strdup("sistema.diagnosticar");
        r.parametros_json = g_strdup("{}");
        r.justificativa = g_strdup("o pedido menciona diagnóstico/sistema");
        r.resposta_texto = g_strdup("(mock) posso rodar um diagnóstico do sistema.");
    } else if (contem_ci(prompt_montado, "ferramenta") || contem_ci(prompt_montado, "descobrir")) {
        r.ferramenta_proposta = g_strdup("tools.discover");
        r.parametros_json = g_strdup("{}");
        r.justificativa = g_strdup("o pedido menciona ferramentas/descoberta");
        r.resposta_texto = g_strdup("(mock) posso inventariar as ferramentas do sistema.");
    } else {
        r.resposta_texto = g_strdup("(mock) não tenho uma ferramenta específica para sugerir aqui.");
    }

    g_free(pedido_isolado);
    concluido_cb(ctx, &r);
    modelo_resposta_liberar(&r);
}

static void mock_cancelar(const ModeloBackend *self, const char *pergunta_id)
{
    (void)self; (void)pergunta_id;
    /* mock_perguntar é síncrona (já terminou antes de cancelar poder
     * chegar) — nada a fazer. */
}

static gboolean mock_disponivel(const ModeloBackend *self, char **detalhe_alocado)
{
    (void)self;
    if (detalhe_alocado) *detalhe_alocado = g_strdup("mock: sempre disponível (determinístico, sem rede)");
    return TRUE;
}

static ModeloBackend MODELO_MOCK = {
    .nome_backend = "mock",
    .perguntar = mock_perguntar,
    .cancelar = mock_cancelar,
    .disponivel = mock_disponivel,
};

const ModeloBackend *modelo_mock_obter(void) { return &MODELO_MOCK; }
