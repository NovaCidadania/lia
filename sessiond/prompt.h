/* lia-sessiond — construção de prompt com fronteiras de confiança
 * explícitas (docs/THREAT-MODEL.md, "dado, nunca instrução"; §5.2).
 *
 * Único lugar do daemon que monta o texto enviado ao modelo. Nunca
 * concatena instrução do sistema com dado externo sem um delimitador
 * claro e rotulado; nunca deixa conteúdo de arquivo aparecer em uma seção
 * que o modelo possa confundir com instrução confiável.
 */
#ifndef LIA_SESSIOND_PROMPT_H
#define LIA_SESSIOND_PROMPT_H

#include <glib.h>

#define LIA_PROMPT_LIMITE_PEDIDO_BYTES      (8 * 1024)
#define LIA_PROMPT_LIMITE_ARQUIVO_BYTES     (16 * 1024)
#define LIA_PROMPT_LIMITE_ARQUIVOS_TOTAL    (3)
#define LIA_PROMPT_LIMITE_CONTEXTO_BYTES    (64 * 1024)   /* soma de todos os arquivos */
#define LIA_PROMPT_LIMITE_PROMPT_FINAL_BYTES (96 * 1024)

typedef struct {
    char *caminho;   /* rótulo informado pelo cliente — NUNCA confiável, só exibido */
    char *conteudo;  /* conteúdo do arquivo — NUNCA confiável */
} PromptArquivo;

PromptArquivo *prompt_arquivo_novo(const char *caminho, const char *conteudo);
void prompt_arquivo_liberar(PromptArquivo *a);

typedef enum {
    PROMPT_OK = 0,
    PROMPT_ERRO_PEDIDO_VAZIO,
    PROMPT_ERRO_PEDIDO_MUITO_GRANDE,
    PROMPT_ERRO_MUITOS_ARQUIVOS,
    PROMPT_ERRO_CONTEXTO_MUITO_GRANDE,
} PromptErro;

const char *prompt_erro_texto(PromptErro e);

/* Monta o prompt final a partir de: instruções do sistema (fixas,
 * hardcoded — nunca passadas por este parâmetro a partir de dado
 * externo), o manifesto das ferramentas disponíveis (fixo, do sistema),
 * o pedido da pessoa usuária (livre, mas é AÇÃO pedida, não dado
 * externo) e uma lista de arquivos (cada um claramente marcado como não
 * confiável). 'arquivos' pode ser NULL. Aplica os limites acima,
 * truncando com aviso visível em vez de falhar silenciosamente, exceto
 * quando o próprio pedido já vem vazio ou os limites "duros" (nº de
 * arquivos) são excedidos — nesses casos retorna erro e *prompt_saida
 * fica NULL.
 *
 * Também faz uma redação best-effort de dados sensíveis óbvios (padrões
 * de linha de auditoria uid=/exec=, caminhos sob ~/.lia/humano) antes de
 * colocar qualquer coisa no prompt — não é uma garantia formal, é defesa
 * em profundidade (ver docs/THREAT-MODEL.md §5.2 para a limitação). */
/* 'manifesto_texto' é construído por quem chama (sessiond.c, a partir do
 * Executor ativo — nunca duplicado/hardcoded aqui, para nunca divergir
 * do manifesto real) e tratado como confiável (vem do próprio sistema,
 * não de fora). */
PromptErro prompt_montar(const char *manifesto_texto, const char *pedido_usuario,
                          GList *arquivos /* PromptArquivo* */, char **prompt_saida);

#endif
