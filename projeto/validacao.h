/* Validação do projeto (build/testes) — comandos nomeados, executados em
 * isolamento (projeto/processo.h) e SEMPRE com aprovação humana.
 *
 * De onde vêm os comandos: <projeto>/.lia/validacao.json
 *   {"comandos": {"teste": ["make", "teste"], "build": {"argv": ["make"], "timeout_segundos": 300}}}
 * ou, sem esse arquivo, detecção (Makefile: "build" = make; alvos
 * teste/test/check). Esse arquivo é DADO NÃO CONFIÁVEL do repositório: ele
 * só nomeia comandos; quem autoriza é a pessoa, vendo a prévia, e a
 * política do daemon (executáveis permitidos, shells e código em linha
 * proibidos, isolamento obrigatório) vale por cima dele.
 *
 * A aprovação fica presa ao hash da prévia, que inclui o argv, o conteúdo
 * da configuração e uma impressão digital do projeto (caminho, tamanho,
 * data e modo de cada arquivo fora de .git/): qualquer mudança entre a
 * aprovação e a execução invalida-a. */
#ifndef LIA_PROJETO_VALIDACAO_H
#define LIA_PROJETO_VALIDACAO_H

#include <glib.h>
#include <json-glib/json-glib.h>

#define VALIDACAO_TIMEOUT_PADRAO_S 120
#define VALIDACAO_TIMEOUT_MAX_S 1800

/* {"isolamento":{"disponivel","detalhe"}, "origem", "comandos":[{nome, argv, executavel, permitido, motivo, timeout_segundos}]} */
char *validacao_listar(char **erro);
/* nome do comando padrão para "rode os testes" (teste/test/check, senão build), ou NULL */
char *validacao_comando_padrao(void);
/* prévia (texto) do que será executado; NULL + erro se não puder rodar */
char *validacao_previa(JsonObject *params, char **erro);
/* executa (bloqueia; chame numa thread). JSON do resultado, ou NULL + erro */
char *validacao_executar(JsonObject *params, volatile gint *cancelado, char **erro);
/* últimas n linhas + até 6 linhas-chave de erro anteriores (Fatal error, Traceback, error:...) */
char *validacao_resumo_falha(const char *t, int n, gsize max);

/* exposto para testes */
gboolean validacao_argv_permitido(char **argv, char **motivo);
char *validacao_impressao_digital(const char *raiz);

#endif
