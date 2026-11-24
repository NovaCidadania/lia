/* Ciclo de coding — partes puras (sem daemon), testáveis sem modelo.
 *
 * Etapas: critérios de aceite → inspeção (leitura dos arquivos) →
 * proposta de edição pelo modelo → aprovação (arquivo.criar/escrever com
 * prévia) → validação (validar.executar com aprovação) → revisão → fim,
 * ou novo ciclo de correção (limitado). Tudo que vem do modelo, dos
 * arquivos, da saída dos testes e da configuração do projeto é DADO não
 * confiável: é validado aqui e de novo pelo daemon antes de qualquer efeito.
 *
 * Edições: o modelo só pode CRIAR arquivo que não existe ou EDITAR
 * (procurar → substituir, trecho único) arquivo que a Lia LEU nesta tarefa;
 * as edições de um arquivo são aplicadas em memória e viram UM
 * arquivo.escrever com sha256_esperado = hash observado na leitura — se o
 * arquivo mudar depois, a gravação é recusada. */
#ifndef LIA_SESSIOND_CODING_H
#define LIA_SESSIOND_CODING_H

#include <glib.h>
#include <json-glib/json-glib.h>

#define CODING_MAX_CICLOS 3
#define CODING_MAX_ARQUIVOS_LIDOS 8
#define CODING_MAX_BYTES_ARQUIVO (128 * 1024)
#define CODING_MAX_BYTES_PROMPT_ARQUIVOS (24 * 1024)

/* "crie src/x.c com um programa que...", "corrija o teste que falha"... */
gboolean coding_e_pedido(const char *pedido);

typedef struct {
    char *descricao;
    char *verificacao; /* "validacao" | "contem" */
    char *caminho;     /* contem */
    char *texto;       /* contem */
} CodingCriterio;

typedef struct {
    GPtrArray *criterios;  /* CodingCriterio* */
    char **descartados;    /* critérios não verificáveis, descartados (para o relatório) */
    char **arquivos_para_ler;
    char *validacao;       /* nome de comando ou NULL */
} CodingPlano;

void coding_plano_liberar(CodingPlano *p);
/* JSON do modelo (tolerante: cercas ```, texto em volta). NULL + erro se não der */
CodingPlano *coding_ler_plano(const char *texto, char **erro);
/* critérios de aceite vêm do PEDIDO: "contem" sobre arquivo que o pedido
 * não cita é descartado (vai para 'descartados', com o motivo) */
void coding_filtrar_criterios(CodingPlano *p, const char *pedido);
/* plano mínimo sem modelo: um critério (a validação passa) e os caminhos do pedido */
CodingPlano *coding_plano_padrao(const char *pedido);

typedef struct {
    char *caminho;
    char *conteudo;   /* conteúdo final completo */
    gboolean existia;
    char *sha_lido;   /* hash observado na leitura (existia) */
    char *antes;      /* conteúdo lido (existia) */
} CodingArquivo;

void coding_arquivo_liberar(gpointer p);
/* Aplica em memória as edições propostas. 'lidos': caminho -> conteúdo;
 * 'shas': caminho -> sha256. Devolve os arquivos finais (só os que mudam)
 * e *hash: identidade da proposta (para detectar repetição). */
GPtrArray *coding_aplicar_edicoes(const char *texto_modelo, GHashTable *lidos, GHashTable *shas, char **hash, char **explicacao, char **erro);

char *coding_prompt_plano(const char *pedido, const char *lista_arquivos);
char *coding_prompt_edicoes(const char *pedido, GPtrArray *criterios, const char *praticas, GHashTable *lidos,
                            const char *falha, const char *erro_anterior, int ciclo);

/* critério conferido de verdade? *como: explicação curta */
gboolean coding_conferir_criterio(const CodingCriterio *c, gboolean validacao_passou, char **como);

/* checkpoint: ~/.lia/projeto/coding/<id>.json (0600), sem conteúdo de
 * arquivo nem saída de testes. 'dados' é o objeto a gravar. */
char *coding_dir_checkpoints(void);
gboolean coding_salvar_checkpoint(const char *id, JsonObject *dados, char **erro);
JsonObject *coding_carregar_checkpoint(const char *id, char **erro);
gboolean coding_id_valido(const char *id);

#endif
