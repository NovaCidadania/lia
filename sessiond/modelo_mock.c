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
#include <stdlib.h>
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
                            const char *prompt_montado_completo, const ModeloOpcoes *opcoes,
                            ModeloConcluidoCb concluido_cb, gpointer ctx)
{
    (void)self; (void)pergunta_id;
    if (opcoes && opcoes->resumo) {
        /* pedido de resumo final (sessiond.c, disparar_resumo_modelo):
         * nunca propõe outra ferramenta. MOCK_RESUMO_INVENTADO no pedido
         * original força, de propósito, um resumo com um número que a
         * ferramenta não devolveu — testa a checagem de fundamentação. */
        ModeloResposta rr = { 0 };
        rr.sucesso = TRUE;
        if (strstr(prompt_montado_completo, "\"linha_do_tempo\"")) {
            /* síntese de pesquisa web (sessiond/sintese_web.h). Marcadores no
             * pedido: MOCK_SINTESE_INVENTADA (números que as fontes não têm),
             * MOCK_SINTESE_INVALIDA (não é JSON). Lentidão/cancelamento são
             * testados com o servidor Ollama falso (modo "lento"), nunca
             * bloqueando o laço principal aqui. */
            const char *s = prompt_montado_completo;
            if (strstr(s, "MOCK_SINTESE_INVALIDA")) {
                rr.resposta_texto = g_strdup("isto não é JSON");
            } else if (strstr(s, "MOCK_SINTESE_INVENTADA")) {
                rr.resposta_texto = g_strdup(
                    "{\"resposta\":\"Foi criada em 1777 por um comitê [1].\",\"pontos\":[{\"texto\":\"A versão 9.99 saiu em 2031.\",\"fontes\":[1]},"
                    "{\"texto\":\"(mock) frase que nenhuma fonte sustenta sobre receitas de bolo.\",\"fontes\":[1]}],"
                    "\"linha_do_tempo\":[{\"quando\":\"2031\",\"evento\":\"lançamento inventado\",\"fontes\":[1]}],\"lacunas\":\"\"}");
            } else {
                /* "bem-comportado": copia a primeira frase de cada fonte do
                 * prompt (linhas "[n] título" seguidas do trecho) como ponto
                 * citado; a terceira cita uma fonte inexistente de propósito
                 * (o daemon precisa reatribuir ou descartar) */
                const char *dados = strstr(s, "----- início dos dados -----");
                char **linhas = g_strsplit(dados ? dados : "", "\n", -1);
                GString *pontos = g_string_new(NULL);
                char *primeira = NULL;
                int n = 0;
                for (int i = 0; linhas[i] && linhas[i + 1] && n < 3; i++) {
                    if (linhas[i][0] != '[' || !g_ascii_isdigit(linhas[i][1])) continue;
                    int fonte = atoi(linhas[i] + 1);
                    char *frase = g_strdup(linhas[i + 1]);
                    char *ponto = strstr(frase, ". ");
                    if (ponto) ponto[1] = '\0';
                    GString *esc = g_string_new(NULL);
                    for (const char *q = frase; *q; q++) { if (*q == '"' || *q == '\\') g_string_append_c(esc, '\\'); g_string_append_c(esc, *q); }
                    if (!primeira) primeira = g_strdup(esc->str);
                    g_string_append_printf(pontos, "%s{\"texto\":\"%s\",\"fontes\":[%d]}", n ? "," : "", esc->str, n == 2 ? 9 : fonte);
                    n++;
                    g_string_free(esc, TRUE);
                    g_free(frase);
                }
                rr.resposta_texto = g_strdup_printf(
                    "{\"resposta\":\"(mock) síntese conclusiva: %s\",\"pontos\":[%s],\"linha_do_tempo\":[],\"lacunas\":\"(mock) nenhuma lacuna relevante.\"}",
                    primeira ? primeira : "sem fontes", pontos->str);
                g_free(primeira);
                g_string_free(pontos, TRUE);
                g_strfreev(linhas);
            }
            concluido_cb(ctx, &rr);
            modelo_resposta_liberar(&rr);
            return;
        }
        if (opcoes->esquema_json) {
            /* seleção extrativa: devolve {"linhas":[...]}; MOCK_SELECAO_INVALIDA
             * força índices fora da faixa (o daemon precisa ignorá-los). */
            rr.resposta_texto = g_strdup(strstr(prompt_montado_completo, "MOCK_SELECAO_INVALIDA")
                ? "{\"linhas\":[999,-3]}" : "{\"linhas\":[1]}");
            concluido_cb(ctx, &rr);
            modelo_resposta_liberar(&rr);
            return;
        }
        rr.resposta_texto = g_strdup(strstr(prompt_montado_completo, "MOCK_RESUMO_INVENTADO")
            ? "(mock) o processo fantasma usa 98765 MiB."
            : strstr(prompt_montado_completo, "MOCK_RESUMO_RECUSA") ? "(mock) Não posso fornecer essa informação."
            : strstr(prompt_montado_completo, "MOCK_RESUMO_SEGURO") ? "(mock) O sistema está livre de vírus e seguro."
            : strstr(prompt_montado_completo, "MOCK_RESUMO_POSITIVO") ? "(mock) O sistema está sem problemas e funcionando normalmente."
            : "(mock) resultado recebido e resumido.");
        concluido_cb(ctx, &rr);
        modelo_resposta_liberar(&rr);
        return;
    }
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

    const char *marcador_plano = strstr(prompt_montado, "MOCK_PLANO:");
    const char *marcador = strstr(prompt_montado, "MOCK_PROPOR:");
    if (marcador_plano) {
        /* "MOCK_PLANO:<json até o fim da linha>": o mock "propõe" esse plano
         * de vários passos tal como veio — inclusive planos inválidos de
         * propósito (ferramenta inventada, passo administrativo, ciclo). */
        marcador_plano += strlen("MOCK_PLANO:");
        const char *fim = strchr(marcador_plano, '\n');
        r.plano_json = fim ? g_strndup(marcador_plano, (gsize)(fim - marcador_plano)) : g_strdup(marcador_plano);
        r.resposta_texto = g_strdup("(mock) proponho um plano de vários passos.");
    } else if (marcador) {
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
    } else if (strstr(prompt_montado, "MOCK_PARAMS_INVALIDOS")) {
        r.ferramenta_proposta = g_strdup("tools.list");
        r.parametros_json = g_strdup("{\"estado\":\"ESTADO_QUE_NAO_EXISTE\"}");
        r.justificativa = g_strdup("parâmetros inválidos forçados por marcador de teste");
        r.resposta_texto = g_strdup("(mock) proposta com parâmetros inválidos.");
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
