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
PromptErro prompt_montar_com_contexto(const char *manifesto_texto, const char *pedido_usuario,
                                      const char *contexto_conversa,
                                      GList *arquivos /* PromptArquivo* */, char **prompt_saida);

/* Acrescenta, ao final de um prompt já montado por prompt_montar, uma
 * seção CONFIÁVEL de correção: usada para a única nova tentativa quando a
 * primeira resposta do modelo foi inválida (ferramenta inexistente,
 * parâmetros inválidos...). 'motivo' vem do validador do próprio daemon —
 * nunca de dado externo. Devolve texto alocado (g_free). */
char *prompt_acrescentar_correcao(const char *prompt_original, const char *motivo);

/* Prompt do RESUMO FINAL: curto de propósito (não repete as instruções de
 * escolha de ferramenta nem o manifesto — o modelo aqui só escreve a
 * resposta). 'dados_reais' é o texto renderizado por
 * sessiond/apresentacao.c a partir do JSON que a ferramenta devolveu:
 * é DADO (vem do sistema: nomes de processo, de dispositivo...), e vai
 * numa seção delimitada e rotulada como tal. Nunca falha: pedido ausente
 * vira "(pedido não disponível)". */
char *prompt_montar_resumo(const char *pedido_usuario, const char *ferramenta, const char *dados_reais);

/* Prompt da resposta EXTRATIVA: o modelo recebe as linhas de dados já
 * numeradas e devolve só {"linhas":[...]} — quais delas respondem à
 * pergunta. Ele não escreve nenhum valor: quem monta o texto final, a
 * partir das linhas escolhidas, é o daemon (sessiond/apresentacao.c). */
char *prompt_montar_selecao(const char *pedido_usuario, const char *linhas_numeradas);

/* Prompt da LEITURA COMPLEMENTAR de uma tarefa de vários passos: o modelo
 * recebe o relatório determinístico (conclusão + evidências já medidas),
 * como DADO delimitado, e escreve no máximo duas frases relacionando as
 * evidências. O relatório já é a resposta; esta leitura é opcional e é
 * conferida depois (sessiond.c, ao_comentario_modelo). */
char *prompt_montar_interpretacao(const char *pedido_usuario, const char *relatorio);

/* Prompt da SÍNTESE de uma pesquisa web (sessiond/sintese_web.h): o modelo
 * recebe as fontes numeradas com o trecho lido de cada página, como DADO
 * delimitado, e devolve JSON estruturado (resposta conclusiva, pontos com
 * citações, linha do tempo, lacunas). 'hoje' (AAAA-MM-DD) situa perguntas
 * sobre o "estado atual". */
char *prompt_montar_sintese_web(const char *pedido_usuario, const char *fontes_numeradas, const char *hoje);

#endif
