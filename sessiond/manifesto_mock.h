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
    const char *risco;           /* nenhum|baixo|medio|alto|admin — docs/manifesto.md §1 */
    gboolean requer_aprovacao;
    const char *reversivel;      /* sim|nao_aplicavel|nao */
    const char *const *capacidades; /* terminada em NULL */
    const char *efeito_simulado;
    int timeout_padrao_ms;
} ManifestoMock;

/* Procura pelo nome exato; NULL se não existir nesta tabela fictícia
 * (equivalente a 'ferramenta_desconhecida' no protocolo real). */
const ManifestoMock *manifesto_mock_buscar(const char *nome);

#endif
