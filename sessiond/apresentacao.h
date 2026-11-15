/* lia-sessiond — apresentação determinística do resultado de uma ferramenta
 * e checagem de fundamentação do resumo do modelo.
 *
 * Duas funções puras (sem E/S, sem socket, sem modelo), testáveis em
 * testes/unit/teste_roteador.c:
 *
 *   apresentacao_renderizar(): transforma o JSON real devolvido pela
 *     ferramenta num texto legível (tabela de processos, linhas de
 *     recursos...). É ESTE texto, derivado só do dado real, que:
 *       - o cliente mostra como resultado (a pessoa vê o dado de verdade,
 *         não uma paráfrase do modelo);
 *       - o modelo recebe para resumir (bem mais fácil de citar certo do
 *         que "mem_pct_centesimos": 1583).
 *
 *   apresentacao_resumo_fundamentado(): confere que todo número citado no
 *     resumo do modelo existe na evidência (texto renderizado + JSON bruto
 *     + pedido da pessoa). Um resumo que cita um número que a ferramenta
 *     não devolveu é descartado pelo daemon e substituído por uma resposta
 *     determinística — o modelo nunca consegue "trocar números".
 */
#ifndef LIA_SESSIOND_APRESENTACAO_H
#define LIA_SESSIOND_APRESENTACAO_H

#include <glib.h>

/* Limite do texto renderizado (também é o que vai para o prompt de resumo). */
#define LIA_APRESENTACAO_LIMITE_BYTES 6000

/* 'dados_json' é o 'dados' do resultado (normalmente {"saida": ...} ou
 * {"texto": "..."}). Nunca devolve NULL: JSON ausente/malformado vira um
 * aviso explícito. *truncado (opcional) = TRUE se o texto foi cortado no
 * limite — o texto já termina com um aviso dizendo isso. */
char *apresentacao_renderizar(const char *ferramenta, const char *dados_json, gboolean *truncado);

/* TRUE se todo número "solto" de 'resumo' aparece em alguma das evidências
 * (comparação numérica: aceita vírgula ou ponto decimal, separador de
 * milhar e arredondamento/truncamento na última casa citada). Dígitos
 * isolados (0–9) e numeração de lista no começo da linha não contam.
 * Em caso de falha, *suspeito (opcional, g_free) recebe o primeiro número
 * sem origem. */
gboolean apresentacao_resumo_fundamentado(const char *resumo, const char *const *evidencias, char **suspeito);

/* Para ferramentas que devolvem um RANKING (processos por CPU/memória): os
 * nomes de processo citados no resumo precisam ser exatamente os primeiros
 * colocados, sem pular ninguém ("A e D são os que mais usam" quando B e C
 * vêm antes de D é uma afirmação falsa, mesmo com os números certos).
 * TRUE também quando a ferramenta não é de ranking ou o resumo não cita
 * nome nenhum. Em caso de falha, *motivo (opcional, g_free) explica. */
gboolean apresentacao_resumo_respeita_ranking(const char *resumo, const char *ferramenta,
                                               const char *dados_json, char **motivo);

/* Resposta final DETERMINÍSTICA, escrita só a partir do JSON real: uma ou
 * duas frases que respondem o essencial (o primeiro colocado e os dois
 * seguintes, a RAM disponível, o kernel...). Usada quando o resumo do
 * modelo não veio ou foi descartado. Nunca NULL; para ferramentas sem
 * frase própria, uma frase genérica apontando para os dados mostrados. */
char *apresentacao_frase(const char *ferramenta, const char *dados_json);

/* Como a resposta final de cada ferramenta é produzida (sessiond.c). O
 * resumo em texto livre de um modelo local pequeno passou nas checagens
 * numéricas e ainda assim afirmou coisas falsas em teste real ("todos os
 * dispositivos PCI são Intel", RAM total citada como "disponível") — por
 * isso só é usado onde não há alternativa, e sempre conferido. */
typedef enum {
    RESPOSTA_DETERMINISTICA = 0, /* ranking/lista: apresentacao_frase(), sem modelo nenhum */
    RESPOSTA_EXTRATIVA,          /* campos heterogêneos: o modelo só ESCOLHE linhas; o texto é copiado dos dados */
    RESPOSTA_RESUMO_CONFERIDO,   /* demais ferramentas: resumo livre do modelo, conferido; reserva determinística */
} RespostaModo;

RespostaModo apresentacao_modo_resposta(const char *ferramenta);

/* Numera as linhas não vazias de 'apresentacao' ("1. RAM: ...") para o
 * prompt de seleção. *n_linhas recebe a quantidade. */
char *apresentacao_numerar_linhas(const char *apresentacao, int *n_linhas);

/* Resposta EXTRATIVA: junta, na ordem original, as linhas de 'apresentacao'
 * que (a) o modelo escolheu ('linhas_modelo', índices a partir de 1; os
 * fora da faixa são ignorados) e (b) casam com um conceito citado no
 * pedido (RAM, swap, uptime, carga, CPU, kernel, distribuição) — a união
 * das duas coisas, para uma escolha ruim do modelo nunca deixar de fora o
 * que foi perguntado. Sem nenhuma linha escolhida por nenhum dos dois,
 * usa todas. Cada palavra do resultado vem do texto renderizado: o modelo
 * não tem como introduzir um valor. *do_modelo (opcional) = quantas linhas
 * válidas vieram da escolha do modelo. Nunca NULL. */
char *apresentacao_resposta_extrativa(const char *apresentacao, const char *pedido,
                                       const int *linhas_modelo, int n_modelo, int *do_modelo);

/* TRUE se o resumo livre é uma recusa/negação ("não posso fornecer", "os
 * dados não incluem...") — inaceitável como resposta final quando a
 * ferramenta executou com sucesso e devolveu dados. */
gboolean apresentacao_resumo_e_recusa(const char *resumo);

#endif
