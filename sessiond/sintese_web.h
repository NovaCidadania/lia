/* lia-sessiond — resposta final de pesquisas web: conclusiva, estruturada e
 * conferida contra as fontes lidas (docs/protocolo.md §12).
 *
 *   resultado de web.pesquisar/web.abrir (JSON)
 *     → fontes numeradas, cada uma com o TRECHO relevante da página
 *     → o modelo raciocina sobre os trechos e devolve JSON estruturado
 *       (resposta conclusiva, pontos principais, linha do tempo, lacunas),
 *       cada item citando as fontes que o sustentam
 *     → o daemon confere cada item (fonte citada existe; todo número/ano do
 *       item aparece nos trechos citados) e descarta o que não confere
 *     → texto final: Resposta / Pontos principais / Linha do tempo /
 *       O que as fontes não esclarecem / Fontes
 *
 * Cotação de câmbio não passa pelo modelo: a resposta sai do JSON da API.
 * Se o modelo falhar, demorar ou devolver algo que não confere, a resposta
 * é EXTRATIVA: as frases mais relevantes dos trechos, organizadas nas
 * mesmas seções e citadas — ainda conclusiva, nunca inventada.
 *
 * Código puro (sem E/S, sem modelo): testes/unit/teste_sintese_web.c. */
#ifndef LIA_SESSIOND_SINTESE_WEB_H
#define LIA_SESSIOND_SINTESE_WEB_H

#include <glib.h>

#define SINTESE_WEB_MAX_FONTES 4
#define SINTESE_WEB_MAX_TRECHO 1800

typedef struct {
    char *titulo;
    char *url;
    char *data;        /* declarada pela página (AAAA-MM-DD), ou NULL */
    char *trecho;      /* texto relevante extraído da página (ou o resumo da busca, se não foi lida) */
    gboolean lida;     /* TRUE: a página foi aberta e o trecho é dela; FALSE: só o resumo do buscador */
    char *erro;        /* por que não foi lida, se tentou */
} FonteWeb;

void fonte_web_liberar(gpointer f);

/* Fontes numeradas (1..n, na ordem do array) a partir dos 'dados' JSON dos
 * passos web concluídos. Prioriza as páginas lidas; completa com resumos de
 * busca. 'pedido' escolhe o trecho relevante de páginas abertas com
 * web.abrir. Nunca NULL (pode vir vazio). */
GPtrArray *sintese_web_fontes(const char *const *dados_json, const char *pedido, int max_fontes);

/* Resposta determinística de cotação, se algum dado tiver "cotacoes";
 * senão NULL. */
char *sintese_web_cotacao(const char *const *dados_json);

/* Fontes numeradas como texto, para o prompt do modelo. */
char *sintese_web_fontes_para_prompt(GPtrArray *fontes);

/* Esquema JSON da síntese (saída estruturada do modelo). */
const char *sintese_web_esquema(void);

/* Confere e renderiza a síntese do modelo. Devolve o texto final, ou NULL
 * se nada nela puder ser usado (JSON inválido, recusa, nenhum item
 * conferido). *descartados = itens removidos por não conferirem; *motivo
 * (g_free) explica o que foi descartado/trocado. */
char *sintese_web_renderizar(const char *json_modelo, GPtrArray *fontes, const char *pedido,
                             int *descartados, char **motivo);

/* Resposta extrativa (sem modelo), nas mesmas seções. 'nota' (opcional)
 * explica por que o modelo não foi usado. Nunca NULL. */
char *sintese_web_extrativa(GPtrArray *fontes, const char *pedido, const char *nota);

/* Resposta quando nada foi encontrado/lido: honesta e com o erro real. */
char *sintese_web_sem_dados(GPtrArray *fontes, const char *pedido, const char *erro);

/* Seção "Fontes:" (numeradas, com URL e data). */
char *sintese_web_secao_fontes(GPtrArray *fontes);

#endif
