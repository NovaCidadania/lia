/* Pedidos em português sobre arquivos do projeto -> plano de um passo.
 *
 * Reconhece, sem modelo, os pedidos explícitos: ler/mostrar, listar,
 * procurar, informações (codificação, tamanho...), criar arquivo ou pasta,
 * substituir trecho, acrescentar, sobrescrever, apagar, mover/renomear,
 * copiar, converter codificação/quebra de linha, tornar executável e
 * desfazer. Caminhos e textos vêm do pedido original (entre aspas/crases,
 * blocos ```...``` ou palavras com cara de caminho: "src/main.c").
 *
 * Só monta o plano: a validação (manifesto, projeto definido, prévia
 * possível) e a aprovação de toda escrita continuam no daemon. */
#ifndef LIA_SESSIOND_INTENCAO_ARQUIVOS_H
#define LIA_SESSIOND_INTENCAO_ARQUIVOS_H

#include "agente.h"

/* NULL quando o pedido não é sobre arquivos. Quando é, mas falta algo
 * (caminho, conteúdo), devolve NULL e preenche *texto_direto com a
 * pergunta de esclarecimento. */
AgentePlano *agente_planejar_arquivos(const char *pedido, char **texto_direto);

/* exposto para testes: trechos entre aspas/crases, na ordem em que aparecem */
char **intencao_trechos_citados(const char *pedido);
/* exposto para testes: palavras com cara de caminho de arquivo */
char **intencao_caminhos(const char *pedido);

#endif
