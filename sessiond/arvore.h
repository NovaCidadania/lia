/* Árvore (DAG) de tarefas de coding — estado, contratos e evidências.
 *
 * Formato versionado ("lia-arvore-tarefas/1"): JSON para estado e
 * contratos, JSONL para eventos; código, patches e logs NUNCA vão no
 * JSON — ficam como artefatos separados, endereçados pelo sha256 do
 * conteúdo (artefato_gravar), e o JSON só guarda o hash.
 *
 * Regras que este módulo garante (testadas em testes/unit/teste_arvore.c):
 *  - ids estáveis e únicos; pai e dependências precisam existir; ciclos
 *    (pai + dependências) são recusados, também ao carregar de JSON;
 *  - cada evidência declara o seu ALCANCE ("validação X saiu 0 para
 *    a.c@1f2e…, depois do patch #3") — não é verdade universal;
 *  - um nó só PASSA com os próprios critérios conferidos e uma validação
 *    que passou DEPOIS do último patch em qualquer ponto da sua subárvore;
 *    nó filho que passou nunca promove o pai; dependência que não passou
 *    bloqueia;
 *  - tentativas têm orçamento; a mesma proposta (mesmo hash) de novo é
 *    "repetição sem progresso" e é recusada;
 *  - tentativas não se sobrescrevem: cada uma guarda os seus artefatos.
 */
#ifndef LIA_SESSIOND_ARVORE_H
#define LIA_SESSIOND_ARVORE_H

#include <glib.h>

#define ARVORE_VERSAO "lia-arvore-tarefas/1"

typedef enum { NO_PENDENTE, NO_EXECUTANDO, NO_BLOQUEADO, NO_FALHOU, NO_PASSOU } NoEstado;
const char *arvore_estado_nome(NoEstado e);

typedef struct {
    char *caminho;
    char *origem;   /* "regra: define validar", "pedido", "modelo (conferido contra a lista)" */
    char *sha256;
    char *trecho;   /* "arquivo inteiro (38 linhas)", "linhas 10–40", "símbolo validar" */
} NoContexto;

typedef struct {
    char *descricao;
    char *origem;   /* "pedido" | "validacao" | "modelo (filtrado)" */
    gboolean conferido, ok;
    char *como;
} NoCriterio;

typedef struct {
    char *tipo;          /* "patch" | "validacao" */
    gint64 seq;          /* ordem global na árvore */
    char *comando;       /* validacao: nome/argv do comando */
    int codigo_saida;
    gboolean passou;
    char **arquivos;     /* arquivos cobertos (relativos) */
    char **hashes_antes; /* paralelos a 'arquivos' ("" = não existia) */
    char **hashes_depois;
    char *artefato;      /* sha256 do patch/log guardado à parte, ou NULL */
    char *alcance;       /* o que ESTA evidência prova, e só isso */
} NoEvidencia;

typedef struct {
    char *id;
    gint64 seq;
    char *hash_proposta;
    char *artefato;      /* sha256 da resposta/proposta guardada à parte */
    char *prompt;        /* sha256 do prompt enviado ao modelo (artefato), ou NULL (receita) */
    char *resultado;     /* "aplicada" | "recusada: motivo" | "pendente" */
} NoTentativa;

typedef struct {
    char *id, *pai;
    char **deps;
    char *objetivo;          /* observável */
    char **escopo;           /* arquivos que o nó pode tocar */
    char *entradas_json, *saidas_json; /* objetos JSON tipados (texto) */
    GPtrArray *contexto, *criterios, *evidencias, *tentativas;
    NoEstado estado;
    char *motivo;
    int max_tentativas;      /* 0 = sem tentativas (nó de etapa) */
    gint64 max_ms, max_tokens, ms_usados, tokens_usados; /* orçamento (0 = sem limite próprio) */
} TarefaNo;

typedef struct {
    char *id;
    GPtrArray *nos;          /* TarefaNo* em ordem de criação */
    gint64 seq;
    GPtrArray *eventos;      /* linhas JSON ainda não gravadas */
} Arvore;

Arvore *arvore_nova(const char *id);
void arvore_liberar(Arvore *a);
TarefaNo *arvore_no(Arvore *a, const char *id);

/* NULL em sucesso; senão o motivo. 'pai' NULL só para a raiz (uma). */
char *arvore_adicionar(Arvore *a, const char *id, const char *pai, const char *const *deps, const char *objetivo, const char *const *escopo);
/* referências inexistentes, mais de uma raiz ou ciclo: o motivo; NULL = ok */
char *arvore_validar(Arvore *a);

void arvore_contexto(Arvore *a, const char *no, const char *caminho, const char *origem, const char *sha256, const char *trecho);
guint arvore_criterio(Arvore *a, const char *no, const char *descricao, const char *origem);
void arvore_conferir(Arvore *a, const char *no, guint indice, gboolean ok, const char *como);

/* nova tentativa: recusa (motivo) se o orçamento acabou ou se a proposta
 * repete exatamente uma anterior do mesmo nó */
char *arvore_tentativa(Arvore *a, const char *no, const char *hash_proposta, const char *artefato);
void arvore_tentativa_resultado(Arvore *a, const char *no, const char *resultado);
void arvore_tentativa_prompt(Arvore *a, const char *no, const char *sha_prompt);
/* orçamento de tempo/tokens: FALSE quando passou do limite do nó */
gboolean arvore_consumir(Arvore *a, const char *no, gint64 ms, gint64 tokens);

/* patch gravado (arquivos e hashes antes/depois): devolve o seq */
gint64 arvore_patch(Arvore *a, const char *no, char **arquivos, char **antes, char **depois, const char *artefato);
/* validação executada sobre os arquivos (hashes no momento) */
gint64 arvore_validacao(Arvore *a, const char *no, const char *comando, int codigo_saida, gboolean passou,
                        char **arquivos, char **hashes, const char *artefato);

/* estado do nó pelas regras do cabeçalho; *motivo explica (alocado) */
NoEstado arvore_avaliar(Arvore *a, const char *no, char **motivo);
void arvore_marcar(Arvore *a, const char *no, NoEstado estado, const char *motivo);

char *arvore_para_json(Arvore *a);
Arvore *arvore_de_json(const char *texto, char **erro);

/* eventos: "tipo", "no", "detalhe" + seq; devolve e esvazia as linhas pendentes */
void arvore_evento(Arvore *a, const char *tipo, const char *no, const char *detalhe);
char *arvore_eventos_pendentes(Arvore *a);

/* artefato endereçado por conteúdo: <dir>/<sha256> (0600, sem sobrescrever);
 * devolve o sha256 ou NULL + *erro */
char *artefato_gravar(const char *dir, const char *conteudo, gsize tamanho, char **erro);

#endif
