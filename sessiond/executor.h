/* lia-sessiond — interface abstrata de executor.
 *
 * Separa, de propósito, quatro responsabilidades que não devem se
 * misturar (docs/protocolo.md §8, PLANO.md §10.9):
 *   - validação de manifesto/parâmetros (`buscar_manifesto`/`validar_parametros`)
 *   - política de capacidades (quem decide isso é sessiond.c, contra o que
 *     o manifesto declara — nenhum backend se autoaprova)
 *   - execução em si (`iniciar`/`cancelar`)
 *   - emissão de eventos (callbacks `progresso_cb`/`concluido_cb`, que
 *     sessiond.c traduz para 'progresso'/'resultado' do protocolo)
 * A auditoria em disco também é inteiramente de sessiond.c, nunca do
 * backend — nenhum Executor decide o que é ou não sensível.
 *
 * Dois backends existem: executor_real.c (sessiond/manifesto_real.c,
 * chama cli/sistema.c e cli/ferramentas.c de verdade) e executor_mock.c
 * (sessiond/manifesto_mock.c, inteiramente simulado, só para os testes
 * unitários do protocolo). O padrão em produção é sempre o real — ver
 * `escolher_executor` em sessiond.c.
 */
#ifndef LIA_SESSIOND_EXECUTOR_H
#define LIA_SESSIOND_EXECUTOR_H

#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct {
    const char *nome;
    const char *risco;            /* nenhum|baixo|medio|alto|admin — docs/manifesto.md §1 */
    gboolean requer_aprovacao;
    const char *reversivel;       /* sim|nao_aplicavel|nao */
    const char * const *capacidades; /* terminada em NULL */
    const char *efeitos;          /* descrição legível, única (docs/manifesto.md exige "nunca vazio") */
    int timeout_padrao_ms;        /* docs/manifesto.md §1 "timeout_padrao_ms" */
} ExecutorManifesto;

typedef struct {
    gboolean sucesso;       /* FALSE => erro_codigo/erro_mensagem preenchidos, dados_json ignorado */
    int codigo_saida;
    char *dados_json;       /* objeto JSON compacto (ex.: "{\"a\":1}"); NULL == "{}" */
    char *erro_codigo;      /* códigos de docs/protocolo.md §5 */
    char *erro_mensagem;
} ExecutorResultado;

void executor_resultado_liberar(ExecutorResultado *r);

typedef void (*ExecutorProgressoCb)(gpointer ctx, double fracao, const char *mensagem);
typedef void (*ExecutorSaidaCb)(gpointer ctx, const char *trecho);
typedef void (*ExecutorConcluidoCb)(gpointer ctx, const ExecutorResultado *r);

typedef struct Executor Executor;
struct Executor {
    const char *nome_backend; /* "real" ou "mock" — só para log/testes */
    /* política desta etapa (PLANO.md §10.9): em produção (real) só
     * capacidades de leitura passam, antes de qualquer validação de
     * parâmetro — ver sessiond.c, tratar_solicitar. O backend mock
     * precisa de FALSE aqui: suas ferramentas fictícias de risco
     * médio/admin (mock.escrita/mock.admin) existem exatamente para
     * testar a máquina de estados de aprovação, que o real nunca alcança
     * (todas as suas seis ferramentas são risco=nenhum). */
    gboolean restringe_a_leitura;

    const ExecutorManifesto *(*buscar_manifesto)(const char *ferramenta);

    /* NULL se os parâmetros são válidos contra o manifesto; senão uma
     * mensagem (alocada, g_free) explicando o que falhou. Nunca decide
     * capacidade/aprovação — só tipo/presença/enum dos parâmetros. */
    char *(*validar_parametros)(const ExecutorManifesto *m, JsonObject *parametros);

    /* Inicia a execução (pode ser assíncrona: thread própria, timer...).
     * progresso_cb pode ser chamado zero ou mais vezes; concluido_cb é
     * chamado exatamente uma vez, sempre (mesmo em erro/cancelamento). Os
     * dois recebem 'ctx' exatamente como passado aqui — o chamador decide
     * o que isso é e como mantém a validade dele (ver sessiond.c,
     * contagem de referências de Cliente). */
    void (*iniciar)(const Executor *self, const char *execucao_id, const ExecutorManifesto *m,
                     JsonObject *parametros, ExecutorSaidaCb saida_cb, ExecutorProgressoCb progresso_cb,
                     ExecutorConcluidoCb concluido_cb, gpointer ctx);

    /* Pede cancelamento cooperativo — não garante interrupção imediata do
     * trabalho em andamento, só que concluido_cb (quando vier) reporte
     * estado "cancelado". Sem efeito se execucao_id já terminou/não existe. */
    void (*cancelar)(const Executor *self, const char *execucao_id);
};

const Executor *executor_real_obter(void);
const Executor *executor_mock_obter(void);

#endif
