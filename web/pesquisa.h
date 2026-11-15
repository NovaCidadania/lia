#ifndef LIA_WEB_PESQUISA_H
#define LIA_WEB_PESQUISA_H

#include <glib.h>

/* Pesquisa somente leitura no DuckDuckGo HTML. O módulo não aceita uma URL
 * arbitrária: isso reduz SSRF e mantém a ferramenta com um único destino
 * conhecido. O resultado é JSON compacto e limitado. */
char *lia_web_pesquisar(const char *consulta, int limite, char **erro_saida);

/* Abre uma página HTTPS pública e devolve texto limitado. Não segue
 * redirecionamentos automaticamente: o destino final precisa ser validado
 * novamente pelo chamador desta função. */
char *lia_web_abrir(const char *url, char **erro_saida);

#endif
