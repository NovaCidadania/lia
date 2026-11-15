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
 * Três backends: modelo_mock.c (determinístico, para teste),
 * modelo_ollama.c (ollama/cliente_ollama.c de verdade) e
 * modelo_llama_cpp.c (ollama/cliente_llama_cpp.c, para um llama-server
 * local servindo qualquer modelo .gguf). Nenhum deles jamais chama
 * funções de aprovação/execução — essas funções nem são visíveis para
 * este módulo (não inclui executor.h nem sessiond.c).
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
    /* TRUE quando o texto do modelo era, inteiro, um objeto JSON no formato
     * estruturado pedido (ver modelo_preencher_proposta_estruturada) —
     * FALSE quando caiu no caminho antigo de "JSON embutido em texto livre"
     * ou quando não havia estrutura nenhuma. Só informativo: a validação
     * (sessiond/roteador.h) é a mesma nos dois casos. */
    gboolean estruturada;
    char *tipo;                 /* "proposta" | "resposta" | "esclarecimento" (só quando estruturada), ou NULL */
    /* plano de VÁRIOS passos sugerido pelo modelo: objeto JSON em texto
     * ({"objetivo":"...","passos":[...]}), ou NULL. É só uma sugestão — a
     * validação integral é de sessiond/agente.c (agente_plano_validar). */
    char *plano_json;
} ModeloResposta;

void modelo_resposta_liberar(ModeloResposta *r);

typedef void (*ModeloConcluidoCb)(gpointer ctx, const ModeloResposta *r);

/* Compartilhado entre backends reais (modelo_ollama.c, modelo_llama_cpp.c):
 * preenche r->resposta_texto com 'texto_completo' e, se o modelo tiver
 * incluído um bloco JSON de "proposta de ferramenta"
 * ({"ferramenta":...,"parametros":{...},"justificativa":...}) em algum
 * ponto do texto, extrai e preenche os campos correspondentes. Nunca
 * interpreta o restante do texto como instrução, nunca chama nada por
 * conta própria com isso (ver comentário no topo deste arquivo). */
void modelo_preencher_proposta_de_texto(ModeloResposta *r, const char *texto_completo);

/* Caminho preferido: 'texto_completo' é, inteiro, um objeto JSON
 *   {"tipo":"proposta|resposta|esclarecimento","ferramenta":"<nome>|nenhuma",
 *    "parametros":{...},"justificativa":"...","resposta":"..."}
 * (é o que o backend pede ao modelo quando ModeloOpcoes.esquema_json não é
 * NULL — saída restrita por esquema, não texto livre). Preenche os campos
 * e marca r->estruturada. Se o texto NÃO for um objeto JSON com esse
 * formato, recai em modelo_preencher_proposta_de_texto (compatibilidade
 * com backends/modelos sem saída estruturada) e r->estruturada fica FALSE.
 * Nunca fabrica nada: campo ausente fica NULL. */
void modelo_preencher_proposta_estruturada(ModeloResposta *r, const char *texto_completo);

/* Como a pergunta deve ser feita. Decidido por sessiond.c (que conhece o
 * manifesto), nunca pelo conteúdo do prompt. */
typedef struct {
    /* Esquema JSON (texto) que a resposta deve obedecer, ou NULL para
     * texto livre (usado no resumo final). Backends que suportam saída
     * estruturada (Ollama "format", llama-server "json_schema") repassam;
     * os demais ignoram. */
    const char *esquema_json;
    int max_tokens;      /* limite de tokens gerados; <= 0 usa o padrão do backend */
    gboolean resumo;     /* TRUE: é o pedido de resumo final de um resultado já executado — nunca propõe ferramenta */
} ModeloOpcoes;

typedef struct ModeloBackend ModeloBackend;
struct ModeloBackend {
    const char *nome_backend; /* "mock" ou "ollama" */

    /* prompt_montado já vem pronto de sessiond/prompt.c, com as seções de
     * confiança delimitadas — este módulo não interpreta nada nele, só
     * entrega ao backend. concluido_cb é chamada exatamente uma vez. */
    void (*perguntar)(const ModeloBackend *self, const char *pergunta_id,
                       const char *prompt_montado, const ModeloOpcoes *opcoes,
                       ModeloConcluidoCb concluido_cb, gpointer ctx);

    /* Cancelamento cooperativo; sem efeito se pergunta_id já terminou. */
    void (*cancelar)(const ModeloBackend *self, const char *pergunta_id);

    /* Diagnóstico rápido para 'lia-chat modelo status' — não envia
     * nenhuma pergunta real, só verifica disponibilidade (ex.: tenta
     * conectar ao Ollama). Síncrono e barato; mock sempre retorna TRUE. */
    gboolean (*disponivel)(const ModeloBackend *self, char **detalhe_alocado);
};

const ModeloBackend *modelo_mock_obter(void);
const ModeloBackend *modelo_ollama_obter(void);
const ModeloBackend *modelo_llama_cpp_obter(void);

#endif
