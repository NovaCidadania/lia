/* Catálogo de boas práticas de programação (arquivos JSON em conhecimento/praticas/).
 *
 * Recuperação determinística, sem modelo, sem rede e sem embeddings: termos
 * de ativação e categoria presentes no texto da tarefa, linguagem e
 * prioridade escolhem POUCAS práticas, dentro de um orçamento de bytes —
 * nunca o catálogo inteiro. O texto recuperado é orientação (dados), nunca
 * instrução de sistema: quem o usa deve apresentá-lo como tal.
 *
 * Extensão por projeto sem editar o núcleo: um segundo arquivo (ex.:
 * <projeto>/.lia/praticas.json) acrescenta práticas; o mesmo id substitui a
 * do núcleo. Cada arquivo é validado por inteiro: um erro de esquema
 * recusa o arquivo todo, com a posição do problema. */
#ifndef LIA_PROJETO_PRATICAS_H
#define LIA_PROJETO_PRATICAS_H

#include <glib.h>

#define PRATICAS_MAX_SELECAO 12
#define PRATICAS_MAX_TEXTO 600     /* maior regra/passo/checagem aceita, em bytes */

typedef struct {
    char *id;
    int versao;
    char *categoria;
    char **linguagens;   /* "*" = qualquer */
    char **ativacao;     /* termos (normalizados: minúsculos, sem acento) */
    char *regra;
    char **passos;
    char **checagens;
    char **anti_padroes;
    char *severidade;    /* "obrigatoria" | "recomendada" */
    int prioridade;      /* 0..100 */
    char **referencias;
    char *origem_arquivo;
} Pratica;

typedef struct PraticasCatalogo PraticasCatalogo;

/* Carrega e valida os arquivos (terminados em NULL; os que não existirem
 * são ignorados, exceto o primeiro). NULL + *erro se algum for inválido. */
PraticasCatalogo *praticas_carregar(const char *const *arquivos, char **erro);
void praticas_liberar(PraticasCatalogo *c);
guint praticas_total(const PraticasCatalogo *c);
const Pratica *praticas_buscar_id(const PraticasCatalogo *c, const char *id);
/* sha256 do conteúdo carregado (para cache/registro de versão) */
const char *praticas_versao(const PraticasCatalogo *c);

/* As práticas mais relevantes para a tarefa (não possui os elementos).
 * 'linguagem' NULL = deduzir do texto ("c", "python", "shell", "sql"...).
 * Vazio quando nada no texto ativa nenhuma prática. */
GPtrArray *praticas_selecionar(const PraticasCatalogo *c, const char *tarefa, const char *linguagem, int max, gsize max_bytes);
/* linguagem deduzida do texto da tarefa, ou NULL */
const char *praticas_linguagem_da_tarefa(const char *tarefa);

/* catálogo do processo (carregado uma vez na inicialização; só leitura
 * depois disso, então pode ser consultado de qualquer thread) */
void praticas_definir_global(PraticasCatalogo *c);
const PraticasCatalogo *praticas_global(void);

/* JSON {"total_no_catalogo", "linguagem", "praticas":[...]} */
char *praticas_para_json(const PraticasCatalogo *c, GPtrArray *sel, const char *linguagem);
/* bloco compacto para prompt de modelo, delimitado como DADOS de orientação */
char *praticas_para_prompt(GPtrArray *sel, gsize max_bytes);

#endif
