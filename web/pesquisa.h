#ifndef LIA_WEB_PESQUISA_H
#define LIA_WEB_PESQUISA_H

#include <glib.h>

/* Pesquisa somente leitura (DuckDuckGo HTML, ou API de câmbio para cotações).
 *
 * Com 'abrir_fontes' > 0, as melhores fontes encontradas (até 4) são abertas
 * em paralelo e cada resultado ganha o TRECHO RELEVANTE da página para a
 * pergunta — é isso que permite responder de forma conclusiva, em vez de
 * devolver só títulos e links. Contrato do JSON devolvido:
 *
 *   { "consulta": "...", "fonte": "DuckDuckGo HTML", "fontes_abertas": 2,
 *     "resultados": [ { "titulo", "url", "resumo",            (sempre)
 *                       "aberta": true|false,
 *                       "conteudo": "trecho relevante",        (se aberta)
 *                       "data": "2024-05-01",                  (se a página declarar)
 *                       "erro_abertura": "..." } ] }           (se falhou)
 *
 * Cotação: { "consulta", "fonte", "cotacoes": [ { "par", "base", "destino",
 * "valor", "data" } ], "resultados": [ ... ] }.
 *
 * Só HTTPS público: redirecionamentos são seguidos à mão (até 3), e cada
 * destino é validado de novo (HTTPS, sem credenciais, sem rede local). */
char *lia_web_pesquisar(const char *consulta, int limite, int abrir_fontes, char **erro_saida);

/* Abre uma página HTTPS pública e devolve
 * { "url", "titulo", "data"?, "conteudo", "bytes_recebidos" } — conteúdo é o
 * texto da página (sem scripts/estilos), com quebras de parágrafo. */
char *lia_web_abrir(const char *url, char **erro_saida);

/* ---- partes puras, testáveis sem rede (testes/unit/teste_sintese_web.c) ---- */

/* HTML -> texto: remove script/style/nav/rodapé, converte blocos em quebras
 * de linha, decodifica entidades, garante UTF-8 válido. */
char *lia_web_html_para_texto(const char *html, gsize tamanho);

/* Título da página (<title> ou og:title), ou NULL. */
char *lia_web_html_titulo(const char *html);

/* Data declarada pela página (article:published_time, datePublished,
 * dateModified, <time datetime>), normalizada para AAAA-MM-DD, ou NULL. */
char *lia_web_html_data(const char *html);

/* Trecho mais relevante de 'texto' para 'consulta': o parágrafo de abertura
 * mais os parágrafos que mais compartilham palavras com a consulta, na ordem
 * original, até 'max_caracteres'. Descarta menus, listas de links e linhas
 * curtas. Nunca NULL. */
char *lia_web_trecho_relevante(const char *texto, const char *consulta, gsize max_caracteres);

/* Palavras significativas de um texto (sem acento, minúsculas, sem palavras
 * vazias), para medir relevância. g_strfreev. */
char **lia_web_termos(const char *texto);

#endif
