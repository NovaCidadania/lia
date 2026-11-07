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
 * Importante: todo esse reconhecimento só olha a parte CONFIÁVEL do
 * prompt (tudo antes do primeiro "### CONTEÚDO DE ARQUIVO" — ver
 * sessiond/prompt.c), nunca o conteúdo de arquivo em si. Isto é
 * deliberado: representa, de forma determinística e testável, a mesma
 * fronteira de confiança que as instruções de sistema pedem a um modelo
 * de verdade para respeitar (dado, nunca instrução) — um mock que
 * reagisse a qualquer coisa em qualquer lugar do prompt não testaria essa
 * fronteira, só a desmentiria.
 */
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

    const char *limite_arquivos = strstr(prompt_montado_completo, "### CONTEÚDO DE ARQUIVO");
    char *copia_confiavel = limite_arquivos
        ? g_strndup(prompt_montado_completo, (gsize)(limite_arquivos - prompt_montado_completo))
        : NULL;
    const char *prompt_montado = copia_confiavel ? copia_confiavel : prompt_montado_completo;

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

    g_free(copia_confiavel);
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
