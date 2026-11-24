/* Git do projeto: consultas (status, diff, log) e commit aprovado.
 *
 * Todo git roda em isolamento (projeto/processo.h): consultas com o
 * projeto inteiro somente leitura; o commit com o projeto e .git/
 * graváveis, mas sem rede, sem /home e com ambiente limpo.
 * Configuração: GIT_CONFIG_NOSYSTEM e GIT_CONFIG_GLOBAL=/dev/null (nada do
 * sistema nem do usuário), e por cima da do repositório: ganchos
 * desativados (core.hooksPath vazio), sem fsmonitor, sem pager, sem diff
 * externo nem textconv, sem assinatura, sem gc automático. Não existe
 * push, reset, clean, rebase, checkout de arquivos nem reescrita de
 * histórico — não há código para isso.
 *
 * O commit só inclui os arquivos listados (mostrados na prévia); o que
 * estava preparado (staged) fora deles continua como estava. */
#ifndef LIA_PROJETO_GIT_H
#define LIA_PROJETO_GIT_H

#include <glib.h>
#include <json-glib/json-glib.h>

/* autor dos commits: "Nome <email>" (LIA_SESSIOND_GIT_AUTOR); NULL = o do repositório */
void git_definir_autor(const char *autor);

char *git_status(volatile gint *cancelado, char **erro);
char *git_diff(JsonObject *params, volatile gint *cancelado, char **erro);
char *git_log(JsonObject *params, volatile gint *cancelado, char **erro);
char *git_commit_previa(JsonObject *params, char **erro);
char *git_commit_aplicar(JsonObject *params, volatile gint *cancelado, char **erro);

#endif
