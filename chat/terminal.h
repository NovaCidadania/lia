/* lia-chat — dono ÚNICO do desenho do terminal interativo.
 *
 * Layout (sem tela cheia: o histórico continua no scrollback normal do
 * terminal, selecionável e rolável como qualquer saída):
 *
 *     ...histórico (perguntas, etapas, dados reais, respostas)...
 *     ⠹ executando leitura · sistema.processos_top_memoria · 3s · Ctrl-C cancela
 *     ──────────────────────────────
 *     lia> texto sendo digitado_
 *
 * As três últimas linhas são a "região viva": status, separador e a linha
 * de entrada. Todo texto novo entra ACIMA dela (term_imprimir apaga a
 * região, escreve o texto e redesenha a região logo abaixo) — a linha de
 * entrada nunca é sobrescrita por saída, e a saída nunca aparece abaixo
 * dela. O que a pessoa já digitou (e a posição do cursor) fica num buffer
 * próprio e é redesenhado como estava.
 *
 * Substitui a libedit usada antes: ela não reiniciava o próprio estado de
 * linha depois do Enter (a pergunta enviada reaparecia no prompt) e não
 * tinha como manter uma linha de status viva acima da entrada. Aqui o
 * editor de linha é pequeno e explícito: UTF-8, setas, Home/End, Delete,
 * Backspace, Ctrl-A/E/B/F/K/U/W/L/D, histórico (↑/↓, só em memória) e
 * colagem (bracketed paste — quebras de linha coladas viram espaço, nunca
 * um envio acidental).
 *
 * Não cria thread, não instala manipulador de sinal: quem chama liga
 * term_entrada_disponivel() ao fd de entrada e term_redimensionado() ao
 * SIGWINCH no laço principal do GLib.
 */
#ifndef LIA_CHAT_TERMINAL_H
#define LIA_CHAT_TERMINAL_H

#include <glib.h>

typedef struct Term Term;

/* linha == NULL significa fim de entrada (Ctrl-D em linha vazia, ou EOF). */
typedef void (*TermLinhaCb)(const char *linha, gpointer dados);

/* Coloca o terminal em modo cru (sem eco, sem modo canônico; ISIG fica
 * ligado: Ctrl-C continua gerando SIGINT) e desenha a região viva. NULL se
 * fd_entrada não for um terminal. */
Term *term_novo(int fd_entrada, int fd_saida, const char *prompt, TermLinhaCb cb, gpointer dados);

/* Apaga a região viva e devolve o terminal ao estado em que estava. */
void term_liberar(Term *t);

/* Escreve 'texto' no histórico, acima da região viva. Pode ter várias
 * linhas e sequências SGR (cores); um '\n' final é opcional. */
void term_imprimir(Term *t, const char *texto);

/* Linha de status. 'ativo' liga o indicador de atividade (spinner) e o
 * tempo decorrido desde esta chamada; com ativo == FALSE mostra só o
 * texto, sem animação. Mudar só o texto mantendo 'ativo' NÃO reinicia o
 * relógio se 'continuar_relogio' for TRUE. */
void term_definir_status(Term *t, const char *texto, gboolean ativo, gboolean continuar_relogio);

void term_definir_prompt(Term *t, const char *prompt);

/* Há bytes para ler em fd_entrada: lê tudo que estiver disponível e
 * processa (pode chamar o callback de linha zero ou mais vezes). Devolve
 * FALSE se não havia nada para ler — chamado com o fd "pronto", isso
 * significa fim de entrada (terminal fechado). */
gboolean term_entrada_disponivel(Term *t);

/* O terminal mudou de tamanho (SIGWINCH): redesenha a região viva. */
void term_redimensionado(Term *t);

/* Limpa a tela visível e redesenha a região viva no topo. */
void term_limpar_tela(Term *t);

/* TRUE se a saída aceita cores (TERM != dumb e NO_COLOR não definido). */
gboolean term_tem_cor(const Term *t);

#endif
