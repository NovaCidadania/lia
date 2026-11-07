/* lia-sessiond — interface abstrata de backend de modelo (LLM local).
 *
 * Simetria deliberada com sessiond/executor.h: separa "o modelo fala" de
 * "o que acontece com o que ele disse". Um ModeloBackend:
 *   - NUNCA recebe acesso ao socket do protocolo, a Cliente, ou a
 *     qualquer função que aprove/execute/audite algo — só texto in,
 *     texto+proposta out;
 *   - NUNCA decide se uma ferramenta é permitida — isso é inteiramente
 *     de sessiond.c/sessiond/executor.h, exatamente como se a proposta
 *     tivesse vindo de um 'solicitar' digitado por uma pessoa;
 *   - a "proposta" de ferramenta que ele retorna é só uma sugestão de
 *     texto (nome + parâmetros + justificativa) — nunca um Plano
 *     validado; só existe um Plano depois que sessiond.c valida contra o
 *     manifesto real, exatamente no mesmo código que valida qualquer
 *     'solicitar' (docs/protocolo.md, docs/THREAT-MODEL.md §5.2).
 *
 * Dois backends: modelo_mock.c (determinístico, para teste) e
 * modelo_ollama.c (ollama/cliente_ollama.c de verdade). Nenhum dos dois
 * jamais chama funções de aprovação/execução — essas funções nem são
 * visíveis para este módulo (não inclui executor.h nem sessiond.c).
 */
#ifndef LIA_SESSIOND_MODELO_H
#define LIA_SESSIOND_MODELO_H

#include <glib.h>

typedef struct {
    gboolean sucesso;
    char *resposta_texto;       /* sempre preenchido quando sucesso */
    char *ferramenta_proposta;  /* nome da ferramenta sugerida, ou NULL (nenhuma) */
    char *parametros_json;      /* objeto JSON compacto dos parâmetros sugeridos, ou NULL */
    char *justificativa;        /* por que o modelo sugere isso, ou NULL */
    char *erro_codigo;          /* se !sucesso */
    char *erro_mensagem;
} ModeloResposta;

void modelo_resposta_liberar(ModeloResposta *r);

typedef void (*ModeloConcluidoCb)(gpointer ctx, const ModeloResposta *r);

typedef struct ModeloBackend ModeloBackend;
struct ModeloBackend {
    const char *nome_backend; /* "mock" ou "ollama" */

    /* prompt_montado já vem pronto de sessiond/prompt.c, com as seções de
     * confiança delimitadas — este módulo não interpreta nada nele, só
     * entrega ao backend. concluido_cb é chamada exatamente uma vez. */
    void (*perguntar)(const ModeloBackend *self, const char *pergunta_id,
                       const char *prompt_montado, ModeloConcluidoCb concluido_cb, gpointer ctx);

    /* Cancelamento cooperativo; sem efeito se pergunta_id já terminou. */
    void (*cancelar)(const ModeloBackend *self, const char *pergunta_id);

    /* Diagnóstico rápido para 'lia-chat modelo status' — não envia
     * nenhuma pergunta real, só verifica disponibilidade (ex.: tenta
     * conectar ao Ollama). Síncrono e barato; mock sempre retorna TRUE. */
    gboolean (*disponivel)(const ModeloBackend *self, char **detalhe_alocado);
};

const ModeloBackend *modelo_mock_obter(void);
const ModeloBackend *modelo_ollama_obter(void);

#endif
