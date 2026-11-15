/* lia-sessiond — roteador de intenção: camada DETERMINÍSTICA entre o que o
 * modelo propõe e o que pode virar um plano (docs/protocolo.md §10,
 * docs/manifesto.md "Roteamento").
 *
 *   pedido da pessoa -> intenção classificada -> ferramenta candidata
 *                    -> validação independente -> (só então) execução
 *
 * Código puro: não faz E/S, não conhece socket, executor nem modelo. Tudo
 * que precisa saber do manifesto chega por RoteadorManifesto (dois
 * ponteiros de função), o que o torna testável sem daemon
 * (testes/unit/teste_roteador.c).
 *
 * O que ele garante, independente do texto que o modelo gerou:
 *   - uma intenção de alta confiança só pode ser atendida por uma
 *     ferramenta da lista de compatíveis daquela intenção (ex.: pergunta
 *     sobre processos nunca sai com sistema.dispositivos nem
 *     sistema.recursos);
 *   - ferramenta inexistente nunca passa;
 *   - parâmetro inválido nunca passa, e nunca é "consertado" por palpite:
 *     ou volta ao padrão do manifesto (objeto vazio / parâmetros fixos da
 *     própria intenção), ou a proposta é rejeitada;
 *   - um inteiro que o modelo colocou num parâmetro sem que esse número
 *     apareça no pedido da pessoa é descartado (o modelo local já
 *     demonstrou inventar "limite": 100 sem ninguém pedir);
 *   - intenção reconhecida para a qual NÃO existe ferramenta (ex.: espaço
 *     livre em disco) remove a proposta em vez de reaproveitar uma
 *     ferramenta parecida.
 */
#ifndef LIA_SESSIOND_ROTEADOR_H
#define LIA_SESSIOND_ROTEADOR_H

#include <glib.h>

typedef enum {
    ROTA_CONFIANCA_NENHUMA = 0, /* intenção não reconhecida: a proposta do modelo só passa pela validação de manifesto */
    ROTA_CONFIANCA_MEDIA,       /* reconhecida, mas mais de uma ferramenta serve: o modelo escolhe DENTRO da lista */
    ROTA_CONFIANCA_ALTA,        /* reconhecida sem ambiguidade */
} RotaConfianca;

#define ROTA_MAX_PERMITIDAS 6

typedef struct {
    const char *intencao;   /* id estável (ex.: "processos_por_memoria"); NULL quando não reconhecida */
    const char *descricao;  /* legível, para a interface ("processos por uso de memória") */
    RotaConfianca confianca;
    const char *ferramenta;       /* canônica; NULL = nenhuma (ver sem_ferramenta/esclarecer) */
    const char *parametros_json;  /* parâmetros fixos da intenção (ex.: {"classe":"rede"}), ou "{}" */
    const char *permitidas[ROTA_MAX_PERMITIDAS + 1]; /* compatíveis, terminada em NULL */
    gboolean sem_ferramenta;      /* intenção reconhecida, mas nenhuma ferramenta existente responde */
    gboolean esclarecer;          /* ambígua: sem escolha válida do modelo, pede esclarecimento */
    const char *mensagem;         /* texto para a pessoa quando sem_ferramenta/esclarecer */
    char parametros_buf[48];      /* armazenamento de parametros_json quando ele depende do pedido (ex.: {"limite":5}) */
} Rota;

/* Classifica o pedido. Considera combinações de conceitos (processo + cpu,
 * consumo + memória + superlativo...), nunca uma palavra isolada. Sempre
 * preenche *out (intencao == NULL quando nada foi reconhecido). */
void roteador_classificar(const char *pedido, Rota *out);

typedef struct {
    gboolean (*existe)(const char *ferramenta, gpointer dados);
    /* NULL se os parâmetros (objeto JSON em texto) são válidos para a
     * ferramenta; senão mensagem alocada (g_free). */
    char *(*validar)(const char *ferramenta, const char *parametros_json, gpointer dados);
    gpointer dados;
} RoteadorManifesto;

typedef enum {
    ROTEADOR_ACEITA = 0,      /* proposta do modelo aceita como veio (ou resposta só em texto) */
    ROTEADOR_CORRIGIDA,       /* ferramenta e/ou parâmetros substituídos pela rota determinística */
    ROTEADOR_SEM_FERRAMENTA,  /* proposta removida: não existe ferramenta para esta intenção */
    ROTEADOR_ESCLARECER,      /* pedido ambíguo: nada é proposto, a pessoa é perguntada */
    ROTEADOR_REPETIR,         /* resposta do modelo inválida: vale UMA nova tentativa com correção */
    ROTEADOR_REJEITADA,       /* resposta inválida mesmo após a tentativa: nada é proposto */
} RoteadorVeredito;

typedef struct {
    RoteadorVeredito veredito;
    char *ferramenta;       /* ferramenta final, ou NULL */
    char *parametros_json;  /* parâmetros finais (objeto JSON), ou NULL */
    char *motivo;           /* explicação curta do que a validação fez (sempre preenchido) */
    char *mensagem;         /* texto para a pessoa (SEM_FERRAMENTA/ESCLARECER/REJEITADA), ou NULL */
} RoteadorDecisao;

void roteador_decisao_liberar(RoteadorDecisao *d);

/* Decide o que fazer com a proposta do modelo ('ferramenta_modelo' pode ser
 * NULL = o modelo não propôs nada). 'pode_repetir' diz se ainda cabe uma
 * nova tentativa ao modelo (só usado quando não há rota de alta confiança
 * para corrigir sozinha). */
void roteador_decidir(const Rota *rota, const char *pedido,
                       const char *ferramenta_modelo, const char *parametros_modelo,
                       const RoteadorManifesto *mf, gboolean pode_repetir,
                       RoteadorDecisao *out);

#endif
