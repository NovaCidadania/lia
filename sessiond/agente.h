/* lia-sessiond — Agent Runtime v2: plano de múltiplos passos, observação,
 * verificação e replanejamento limitado (docs/protocolo.md §11).
 *
 *   objetivo → plano estruturado → validação independente → execução de passos
 *            → observação → verificação dos critérios → replanejamento limitado
 *            → resposta final fundamentada
 *
 * Este módulo é a parte PURA do runtime: estruturas do plano, validação
 * (ferramenta, parâmetros, capacidades, dependências, orçamento, risco
 * agregado, hash), ordem de execução, extração de evidências a partir do
 * JSON real de cada ferramenta, verificador, replanejador determinístico
 * e o relatório final. Não faz E/S, não conhece socket, executor nem
 * modelo — o que precisa do manifesto chega por AgenteManifesto. Por isso
 * é testável sem daemon (testes/unit/teste_agente.c). Quem executa de
 * verdade, com temporizadores e cancelamento, é sessiond/sessiond.c.
 *
 * A autonomia vem do runtime, não de dar mais liberdade ao modelo:
 *   - o plano é validado INTEIRO antes de qualquer passo rodar;
 *   - o verificador olha o JSON das ferramentas (campos, tipos, limites),
 *     nunca um texto do modelo dizendo que deu certo;
 *   - o replanejamento tem orçamento fixo e só acrescenta leitura pura;
 *   - dado devolvido por ferramenta nunca vira passo nem instrução;
 *   - sem evidência, o resultado é "incompleto" — nunca uma conclusão.
 */
#ifndef LIA_SESSIOND_AGENTE_H
#define LIA_SESSIOND_AGENTE_H

#include <glib.h>

#define AGENTE_MAX_PASSOS          8      /* passos por plano, contando os de replanejamento */
#define AGENTE_MAX_REPLANEJAMENTOS 3
#define AGENTE_ORCAMENTO_TEMPO_MS  60000  /* por tarefa de leitura */
/* teto para tarefas com passos longos declarados no manifesto (build/testes) */
#define AGENTE_ORCAMENTO_TEMPO_MAX_MS (40 * 60 * 1000)
#define AGENTE_MAX_PARALELO        4      /* passos de leitura pura simultâneos */
#define AGENTE_MAX_TENTATIVAS_IGUAIS 2    /* mesma ferramenta+parâmetros: no máx. duas vezes (impede repetição infinita) */

typedef enum {
    PASSO_AGUARDANDO = 0,
    PASSO_EXECUTANDO,
    PASSO_SUCESSO,
    PASSO_FALHA,
    PASSO_CANCELADO,
    PASSO_PULADO,     /* dependência falhou, ou a tarefa acabou antes */
} PassoEstado;

const char *agente_passo_estado_texto(PassoEstado e);

typedef struct {
    /* --- plano (imutável depois de validado) --- */
    char *id;
    char *ferramenta;
    char *parametros_json;       /* objeto JSON em texto */
    char **depende_de;           /* ids, terminada em NULL */
    char *objetivo;
    char **evidencia_esperada;   /* nomes de evidência, terminada em NULL */
    int timeout_ms;
    gboolean permitir_falha;
    gboolean de_replanejamento;  /* acrescentado por um ciclo de replanejamento */
    gboolean somente_leitura;    /* do manifesto, preenchido na validação */
    gboolean requer_aprovacao;
    /* --- observação (preenchida pela execução) --- */
    PassoEstado estado;
    char *dados_json;            /* JSON bruto devolvido pela ferramenta */
    char *apresentacao;          /* texto legível derivado só de dados_json */
    char **evidencias;           /* evidências extraídas e conferidas */
    char **limitacoes;
    char *erro;
    gint64 duracao_ms;
} AgentePasso;

typedef enum {
    COMPAT_COMPATIVEL = 0,   /* há ferramentas que respondem o pedido */
    COMPAT_PARCIAL,          /* dá para coletar evidências, mas elas NÃO respondem a pergunta de fundo */
    COMPAT_INEXISTENTE,      /* nenhuma ferramenta desta versão serve */
    COMPAT_ESCLARECER,       /* o pedido precisa ser esclarecido antes */
} AgenteCompat;

const char *agente_compat_texto(AgenteCompat c);

typedef struct {
    char *plano_id;
    char *objetivo;
    char *intencao;              /* id da intenção de tarefa, ou "plano_do_modelo"/"plano_do_cliente" */
    GPtrArray *passos;           /* AgentePasso* */
    char **criterios_sucesso;    /* evidências exigidas para a tarefa contar como concluída */
    char **capacidades;          /* união das capacidades dos passos */
    char *risco;                 /* maior risco entre os passos */
    gboolean requer_aprovacao;   /* TRUE se QUALQUER passo exigir */
    gboolean somente_leitura;    /* TRUE só se TODOS os passos forem leitura pura */
    int orcamento_passos;
    int orcamento_tempo_ms;
    int versao;                  /* 1; +1 a cada replanejamento */
    int replanejamentos;
    char *hash;                  /* SHA-256 do plano validado (versão atual) */
    AgenteCompat compatibilidade;
    char **limitacoes;           /* limitações conhecidas de antemão (ex.: sem ferramenta de malware) */
} AgentePlano;

AgentePlano *agente_plano_novo(const char *objetivo, const char *intencao);
void agente_plano_liberar(AgentePlano *p);

/* Acrescenta um passo ainda não validado. 'depende_de'/'evidencias' podem
 * ser NULL; 'id' NULL gera "passo-N". Devolve o passo (do plano). */
AgentePasso *agente_plano_acrescentar(AgentePlano *p, const char *id, const char *ferramenta,
                                       const char *parametros_json, const char *const *depende_de,
                                       const char *objetivo, const char *const *evidencias,
                                       int timeout_ms, gboolean permitir_falha);

/* ---- o que o runtime precisa saber do manifesto ativo ---- */
typedef struct {
    const char *risco;
    gboolean requer_aprovacao;
    gboolean somente_leitura;
    gboolean capacidade_permitida; /* política de capacidades do executor ativo */
    const char *const *capacidades;
    int timeout_padrao_ms;
} AgenteFerramentaInfo;

typedef struct {
    /* FALSE se a ferramenta não existe */
    gboolean (*info)(const char *ferramenta, AgenteFerramentaInfo *out, gpointer dados);
    /* NULL se os parâmetros são válidos; senão mensagem alocada */
    char *(*validar)(const char *ferramenta, const char *parametros_json, gpointer dados);
    gpointer dados;
} AgenteManifesto;

/* Valida o plano INTEIRO (nenhum passo roda antes disto): limites, ids
 * únicos, ferramenta existente, capacidade permitida, parâmetros,
 * dependências (existentes, sem ciclo), timeouts. Em sucesso preenche
 * risco agregado, capacidades, requer_aprovacao, somente_leitura,
 * critérios (se vazios) e o hash, e devolve NULL. Em erro devolve
 * "codigo: mensagem" alocado ("ferramenta_desconhecida: ...",
 * "capacidade_negada: ...", "parametro_invalido: ...",
 * "plano_invalido: ..."). */
char *agente_plano_validar(AgentePlano *p, const AgenteManifesto *mf);

/* Constrói um plano a partir de JSON vindo de FORA (cliente ou modelo):
 * {"objetivo":"...","passos":[{"id","ferramenta","parametros","depende_de",
 * "objetivo","evidencia_esperada","timeout_ms","permitir_falha"}],
 * "criterios_sucesso":[...]}. Só a forma; a validação é agente_plano_validar.
 * NULL + *erro em JSON malformado. */
AgentePlano *agente_plano_de_json(const char *json, const char *intencao, char **erro);

/* Serializa o plano (com 'com_estado', também a observação de cada passo). */
char *agente_plano_para_json(const AgentePlano *p, gboolean com_estado);

/* Planejador DETERMINÍSTICO: reconhece intenções de tarefa (lentidão,
 * comparação CPU×memória, relatório de recursos, consumo anormal,
 * inventário de hardware, estado geral, evidências sobre malware) e monta
 * o plano de vários passos. Devolve:
 *   - plano (a validar) quando há o que executar;
 *   - NULL e *texto_direto preenchido quando a resposta certa é só texto
 *     (pergunta de sim/não sobre vírus, explicação do que falta);
 *   - NULL e *texto_direto NULL quando o pedido não é uma tarefa composta
 *     (segue o fluxo de consulta simples).
 * *compat sempre preenchido. */
AgentePlano *agente_planejar(const char *pedido, char **texto_direto, AgenteCompat *compat);
/* Variante que usa turnos anteriores apenas como contexto para referências
 * e expansão de consultas web; o pedido atual continua sendo a ação principal. */
AgentePlano *agente_planejar_com_contexto(const char *pedido, const char *contexto,
                                           char **texto_direto, AgenteCompat *compat);

/* Passos que podem começar agora: AGUARDANDO com todas as dependências em
 * SUCESSO. Marca como PULADO (e conta em *pulados) o passo cuja dependência
 * terminou sem sucesso. Devolve índices; no máximo 'max'. Só leitura pura
 * roda em paralelo: se o primeiro pronto não for leitura pura, devolve só
 * ele, e só quando nada mais está executando. */
int agente_passos_prontos(AgentePlano *p, int *indices, int max, int *pulados);

/* Observação: confere o JSON real (campos, tipos, limites) e devolve as
 * evidências que ele de fato sustenta. *limitacoes (opcional) recebe o que
 * veio parcial/ausente. Nunca devolve evidência para dado ausente. */
char **agente_extrair_evidencias(const char *ferramenta, const char *parametros_json,
                                  const char *dados_json, char ***limitacoes);

typedef enum {
    VEREDITO_SUCESSO = 0,  /* todos os critérios têm evidência */
    VEREDITO_INCOMPLETO,   /* há dados, faltam evidências, e não há mais como buscar */
    VEREDITO_REPLANEJAR,   /* faltam evidências e ainda há orçamento */
    VEREDITO_FALHA,        /* nada foi coletado */
} AgenteVeredito;

const char *agente_veredito_texto(AgenteVeredito v);

/* Verificador: olha só evidências extraídas dos dados. *ausentes (g_strfreev)
 * recebe os critérios sem evidência. 'pode_replanejar' vem do runtime
 * (orçamento de ciclos, passos e tempo). */
AgenteVeredito agente_verificar(const AgentePlano *p, gboolean pode_replanejar, char ***ausentes);

/* Replanejador determinístico: para cada evidência ausente, acrescenta um
 * passo de leitura pura de uma ferramenta que a fornece — sem repetir a
 * mesma ferramenta+parâmetros além de AGENTE_MAX_TENTATIVAS_IGUAIS, sem
 * passar de AGENTE_MAX_PASSOS. Devolve quantos passos acrescentou (0 = não
 * há mais o que tentar). Incrementa versão/replanejamentos e recalcula o
 * hash quando acrescenta. */
int agente_replanejar(AgentePlano *p, char **ausentes, const AgenteManifesto *mf);

/* Acrescenta UM passo sugerido de fora (modelo) durante o replanejamento.
 * Só aceita ferramenta existente, de leitura pura, sem exigência de
 * aprovação, com parâmetros válidos e ainda não repetida demais. NULL em
 * sucesso; senão o motivo da recusa (alocado). */
char *agente_replanejar_com_sugestao(AgentePlano *p, const char *ferramenta, const char *parametros_json,
                                      const AgenteManifesto *mf);

/* Relatório final determinístico (Conclusão, Evidências, Limitações, Ações
 * realizadas, Ações não realizadas, Próximo passo), escrito só a partir
 * das observações. 'comentario_modelo' (opcional) entra numa seção própria,
 * rotulada. Nunca NULL. */
char *agente_relatorio(const AgentePlano *p, AgenteVeredito v, char **ausentes, const char *comentario_modelo);

/* Texto com as evidências coletadas, para o prompt de interpretação do
 * modelo e para conferir o que ele escrever. */
char *agente_evidencias_texto(const AgentePlano *p);

/* TRUE se 'texto' faz uma afirmação absoluta de segurança/infecção ("está
 * livre de vírus", "não há malware", "está infectado") — nunca aceitável
 * sem ferramenta de detecção. */
gboolean agente_afirmacao_de_seguranca(const char *texto);

/* TRUE se 'texto' conclui que "está tudo bem" ("sem problemas",
 * "funcionando normalmente", "nada de errado"): ausência de indício nas
 * medições não autoriza essa conclusão. */
gboolean agente_conclusao_positiva_sem_base(const char *texto);

#endif
