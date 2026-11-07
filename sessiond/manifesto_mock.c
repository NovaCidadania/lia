#include "manifesto_mock.h"
#include <string.h>

static const char *CAP_LEITURA[] = { "leitura_arquivo", NULL };
static const char *CAP_ESCRITA[] = { "escrita_arquivo", NULL };
static const char *CAP_ADMIN[] = { "admin", NULL };

static const ManifestoMock TABELA[] = {
    {
        .nome = "mock.leitura",
        .risco = "nenhum",
        .requer_aprovacao = FALSE,
        .reversivel = "nao_aplicavel",
        .capacidades = CAP_LEITURA,
        .efeito_simulado = "simulação: leria um arquivo (nenhum efeito real)",
    },
    {
        .nome = "mock.escrita",
        .risco = "medio",
        .requer_aprovacao = TRUE,
        .reversivel = "sim",
        .capacidades = CAP_ESCRITA,
        .efeito_simulado = "simulação: escreveria/alteraria um arquivo (nenhum efeito real)",
    },
    {
        .nome = "mock.admin",
        .risco = "admin",
        .requer_aprovacao = TRUE,
        .reversivel = "nao",
        .capacidades = CAP_ADMIN,
        .efeito_simulado = "simulação: pediria privilégio admin (nenhum efeito real, nenhum sudo/pkexec é chamado)",
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
