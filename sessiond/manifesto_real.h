/* lia-sessiond — manifesto real das seis ferramentas de leitura permitidas
 * nesta etapa (docs/manifesto.md). Nenhuma delas tem `risco` acima de
 * `nenhum`: todas são somente leitura, sem efeito observável fora do
 * processo, e por isso `requer_aprovacao = false` em todas — a única
 * exceção concedida nesta etapa a "nunca executar comandos externos" é
 * `tools.probe`, que executa o catálogo de probes seguros já existente em
 * cli/ferramentas.c (sandboxed, com timeout, sem shell) — ver
 * docs/protocolo.md §8 e docs/THREAT-MODEL.md §5.1 para a justificativa
 * completa dessa exceção escopada.
 */
#ifndef LIA_SESSIOND_MANIFESTO_REAL_H
#define LIA_SESSIOND_MANIFESTO_REAL_H

#include "executor.h"

typedef enum {
    PARAM_STRING,
    PARAM_BOOLEANO,
    PARAM_ENUM,
} ParametroTipo;

typedef struct {
    const char *nome;
    ParametroTipo tipo;
    gboolean obrigatorio;
    const char * const *valores_enum; /* só para PARAM_ENUM; terminada em NULL */
} ParametroManifesto;

typedef struct {
    ExecutorManifesto base;
    const ParametroManifesto *parametros; /* terminada em {.nome = NULL} */
} ManifestoReal;

const ManifestoReal *manifesto_real_buscar(const char *nome);

/* Terminada em NULL, não possui — só os nomes, para o prompt do modelo
 * (sessiond/prompt.h); nunca usada para decidir nada sobre execução. */
const char * const *manifesto_real_listar_nomes(void);

/* NULL se ok; senão mensagem alocada (g_free) explicando o problema. Só
 * checa forma dos parâmetros (tipo/enum/chave desconhecida) — nunca
 * decide capacidade ou aprovação. */
char *manifesto_real_validar_parametros(const ManifestoReal *m, JsonObject *parametros);

#endif
