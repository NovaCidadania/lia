/* lia-sessiond — tabela de ferramentas simuladas (docs/manifesto.md).
 *
 * Isto NÃO é o manifesto real de ferramentas da Lia — é um conjunto fixo e
 * fictício, só para exercitar a máquina de estados do protocolo (plano ->
 * aprovação -> execução simulada -> resultado) sem nenhum executor real.
 * Nenhuma entrada aqui corresponde a uma ação que de fato acontece no
 * sistema. Ver docs/protocolo.md e sessiond/sessiond.c.
 */
#ifndef LIA_SESSIOND_MANIFESTO_MOCK_H
#define LIA_SESSIOND_MANIFESTO_MOCK_H

#include <glib.h>

typedef struct {
    const char *nome;
    const char *versao;
    const char *risco;           /* nenhum|baixo|medio|alto|admin — docs/manifesto.md §1 */
    gboolean requer_aprovacao;
    gboolean idempotente;
    const char *reversivel;      /* sim|nao_aplicavel|nao */
    const char *const *capacidades; /* terminada em NULL */
    const char *efeito_simulado;
    int timeout_padrao_ms;
    /* ver executor_mock.c: comportamento especial para as ferramentas de
     * teste de limite (nunca conclui / sempre reporta limite_excedido). */
    gboolean nunca_conclui;       /* mock.timeout — só o watchdog de timeout do daemon resolve */
    gboolean forcar_limite_saida; /* mock.saida_grande — simula 'limite_excedido' sem gerar dados de verdade */
} ManifestoMock;

/* Procura pelo nome exato; NULL se não existir nesta tabela fictícia
 * (equivalente a 'ferramenta_desconhecida' no protocolo real). */
const ManifestoMock *manifesto_mock_buscar(const char *nome);

#endif
