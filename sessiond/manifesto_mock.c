#include "manifesto_mock.h"
#include <string.h>

static const char *CAP_LEITURA[] = { "leitura_arquivo", NULL };
static const char *CAP_ESCRITA[] = { "escrita_arquivo", NULL };
static const char *CAP_ADMIN[] = { "admin", NULL };

static const ManifestoMock TABELA[] = {
    {
        .nome = "mock.leitura",
        .versao = "0.1.0",
        .risco = "nenhum",
        .requer_aprovacao = FALSE,
        .idempotente = TRUE,
        .reversivel = "nao_aplicavel",
        .capacidades = CAP_LEITURA,
        .efeito_simulado = "simulação: leria um arquivo (nenhum efeito real)",
        .timeout_padrao_ms = 5000,
    },
    {
        .nome = "mock.escrita",
        .versao = "0.1.0",
        .risco = "medio",
        .requer_aprovacao = TRUE,
        .idempotente = FALSE,
        .reversivel = "sim",
        .capacidades = CAP_ESCRITA,
        .efeito_simulado = "simulação: escreveria/alteraria um arquivo (nenhum efeito real)",
        .timeout_padrao_ms = 5000,
    },
    {
        .nome = "mock.admin",
        .versao = "0.1.0",
        .risco = "admin",
        .requer_aprovacao = TRUE,
        .idempotente = FALSE,
        .reversivel = "nao",
        .capacidades = CAP_ADMIN,
        .efeito_simulado = "simulação: pediria privilégio admin (nenhum efeito real, nenhum sudo/pkexec é chamado)",
        .timeout_padrao_ms = 5000,
    },
    {
        /* NUNCA conclui por si só — só existe para testar o watchdog de
         * timeout do daemon de verdade disparando (sessiond.c,
         * ao_timeout_execucao). timeout_padrao_ms bem curto de propósito,
         * para o teste não precisar esperar. */
        .nome = "mock.timeout",
        .versao = "0.1.0",
        .risco = "nenhum",
        .requer_aprovacao = FALSE,
        .idempotente = TRUE,
        .reversivel = "nao_aplicavel",
        .capacidades = CAP_LEITURA,
        .efeito_simulado = "simulação: nunca termina por si só (só para testar timeout)",
        .timeout_padrao_ms = 300,
        .nunca_conclui = TRUE,
    },
    {
        /* Reporta 'limite_excedido' deterministicamente, sem gerar
         * nenhum dado de verdade — testa o caminho de erro de limite de
         * saída em qualquer backend sem desperdiçar memória/tempo. */
        .nome = "mock.saida_grande",
        .versao = "0.1.0",
        .risco = "nenhum",
        .requer_aprovacao = FALSE,
        .idempotente = TRUE,
        .reversivel = "nao_aplicavel",
        .capacidades = CAP_LEITURA,
        .efeito_simulado = "simulação: saída excederia o limite configurado",
        .timeout_padrao_ms = 5000,
        .forcar_limite_saida = TRUE,
    },
};
#define N_TABELA (sizeof(TABELA) / sizeof(TABELA[0]))

const ManifestoMock *manifesto_mock_buscar(const char *nome)
{
    if (!nome)
        return NULL;
    for (size_t i = 0; i < N_TABELA; i++)
        if (strcmp(TABELA[i].nome, nome) == 0)
            return &TABELA[i];
    return NULL;
}
