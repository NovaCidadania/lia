/* Lia — modelo e validação do protocolo NDJSON (docs/protocolo.md).
 *
 * Camada pura, sem GTK: só GLib/json-glib. Testável sem display (ver
 * gui/teste_protocolo.c, `make teste-gui`). A GUI (gui/lia-gui.c) usa isto
 * para interpretar o que o daemon envia e para recusar, antes de qualquer
 * tentativa de envio, o que não é permitido nesta etapa (modo somente
 * leitura). Nenhuma decisão de autorização real é tomada aqui — isso é
 * sempre do daemon/executor (docs/manifesto.md §5); esta camada só detecta
 * mensagens malformadas, fora de ordem, ou que a própria GUI não deveria
 * enviar.
 */
#ifndef LIA_GUI_PROTOCOLO_H
#define LIA_GUI_PROTOCOLO_H

#include <stddef.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#define LIA_PROTOCOLO_VERSAO "lia.v1"

typedef enum {
    LIA_MSG_DESCONHECIDO = 0,
    /* cliente -> daemon */
    LIA_MSG_SOLICITAR,
    LIA_MSG_APROVAR,
    LIA_MSG_CANCELAR,
    LIA_MSG_ASSINAR,
    LIA_MSG_PERGUNTAR,    /* fala com o modelo — nunca aprova nem executa nada por si só */
    /* daemon -> cliente */
    LIA_MSG_PLANO,
    LIA_MSG_PROGRESSO,
    LIA_MSG_SAIDA,
    LIA_MSG_RESULTADO,
    LIA_MSG_AUDITORIA,
    LIA_MSG_ERRO,
    LIA_MSG_PROPOSTA,     /* resposta do modelo a 'perguntar' — texto + no máx. uma sugestão de ferramenta, nunca um plano */
    LIA_MSG_RESPOSTA,     /* resumo do modelo após um resultado de execução que se originou de uma proposta sua */
} LiaMsgTipo;

typedef enum {
    LIA_MSG_OK = 0,
    LIA_MSG_ERRO_JSON_INVALIDO,       /* não é uma linha JSON válida */
    LIA_MSG_ERRO_ENVELOPE_INVALIDO,   /* falta protocolo/tipo/id/ts, ou "dados" não é objeto */
    LIA_MSG_ERRO_PROTOCOLO_INCOMPATIVEL, /* "protocolo" != "lia.v1" */
    LIA_MSG_ERRO_TIPO_DESCONHECIDO,   /* "tipo" não é nenhum dos listados em docs/protocolo.md §3 */
} LiaMsgErro;

typedef struct {
    LiaMsgTipo tipo;
    char *id;              /* nunca NULL após parse com sucesso */
    char *em_resposta_a;    /* pode ser NULL ("null" ou ausente) */
    char *ts;
    JsonObject *dados;      /* referência; vive enquanto lia_msg_liberar não for chamado */
    JsonParser *_parser;    /* dono da memória de tudo acima; interno */
} LiaMsg;

LiaMsgTipo lia_msg_tipo_de_string(const char *s);
const char *lia_msg_tipo_para_string(LiaMsgTipo t);
const char *lia_msg_erro_texto(LiaMsgErro e);

/* Interpreta uma linha NDJSON. Em caso de erro, *out não precisa ser
 * liberado (nada foi alocado que sobreviva). Em caso de sucesso, quem
 * chamar deve, depois de usar, chamar lia_msg_liberar(out). */
LiaMsgErro lia_msg_parsear(const char *linha, LiaMsg *out);
void lia_msg_liberar(LiaMsg *m);

/* ---------------------------------------------------------------------- */
/* Construção de mensagens — usado tanto pela GUI (gui/lia-gui.c, envia
 * 'solicitar'/'aprovar'/'cancelar'/'assinar') quanto pelo daemon
 * (sessiond/sessiond.c, envia 'plano'/'progresso'/'saida'/'resultado'/
 * 'auditoria'/'erro'). Mantém um único lugar que sabe montar o envelope do
 * protocolo (docs/protocolo.md §2), para os dois lados nunca divergirem. */

/* RFC 3339 UTC, com 'Z' (ex.: "2026-11-07T10:00:00Z"). Resultado alocado
 * (g_free). */
char *lia_ts_agora(void);

/* Identificador único o bastante para esta sessão/processo (não é
 * ULID/UUID formal, mas monotônico + contador — suficiente para o
 * protocolo, que só exige unicidade dentro da sessão). Resultado alocado
 * (g_free). */
char *lia_novo_id(const char *prefixo);

/* Monta uma linha NDJSON completa (envelope + "\n"), pronta para escrever
 * no socket. 'dados', se não for NULL, deve já estar com
 * json_builder_begin_object()/.../json_builder_end_object() chamados; esta
 * função usa a raiz dele e NÃO libera 'dados' — quem chamou continua dono
 * do JsonBuilder e deve g_object_unref nele depois. Resultado alocado
 * (g_free). */
char *lia_msg_montar(LiaMsgTipo tipo, const char *id, const char *em_resposta_a, JsonBuilder *dados);

/* ---------------------------------------------------------------------- */
/* Sessão: estado mínimo do lado do cliente para detectar mensagens fora
 * de ordem e para recusar, localmente, qualquer envio que a GUI não deva
 * fazer nesta etapa (modo_leitura). Ver docs/protocolo.md §3-4. */

typedef struct {
    gboolean conectado;
    gboolean modo_leitura;        /* true nesta etapa: nunca envia 'aprovar' (ver lia_sessao_validar_envio) */
    GHashTable *ids_vistos;       /* id (string) -> gboolean, para checar em_resposta_a */
    GHashTable *execucoes;        /* execucao_id (string) -> estado (LiaExecEstado*) */
} LiaSessao;

typedef enum {
    LIA_EXEC_EM_ANDAMENTO = 0,
    LIA_EXEC_CONCLUIDA,
} LiaExecEstado;

typedef enum {
    LIA_SESSAO_OK = 0,
    LIA_SESSAO_ERRO_RESPOSTA_DESCONHECIDA, /* em_resposta_a referencia id nunca visto */
    LIA_SESSAO_ERRO_EXECUCAO_JA_CONCLUIDA, /* progresso/saida/resultado após resultado final */
} LiaSessaoErro;

void lia_sessao_iniciar(LiaSessao *s);
void lia_sessao_liberar(LiaSessao *s);
void lia_sessao_conectar(LiaSessao *s);
void lia_sessao_desconectar(LiaSessao *s); /* preserva estado de execução pendente */

/* Atualiza o estado da sessão a partir de uma mensagem do daemon já
 * validada por lia_msg_parsear. Retorna LIA_SESSAO_OK se a mensagem é
 * aceita na ordem em que chegou; qualquer outro valor é uma violação de
 * protocolo que a GUI deve mostrar como erro, nunca aplicar ao estado. */
LiaSessaoErro lia_sessao_receber(LiaSessao *s, const LiaMsg *m);

typedef enum {
    LIA_ENVIO_OK = 0,
    LIA_ENVIO_RECUSADO_SOMENTE_LEITURA,   /* 'aprovar' com modo_leitura ativo */
    LIA_ENVIO_RECUSADO_CANAL_DESCONHECIDO, /* 'assinar' com canal fora de {"auditoria","execucao"} */
    LIA_ENVIO_RECUSADO_NADA_PARA_CANCELAR, /* 'cancelar' sem execução em andamento */
} LiaEnvioErro;

/* 'perguntar' é sempre permitido (não tem efeito — é conversa com o
 * modelo); 'proposta'/'resposta' do daemon são sempre aceitas pelo
 * cliente como qualquer outra mensagem informativa. Nenhuma trava extra
 * de lia_sessao_validar_envio é necessária para esses dois tipos. */

/* Valida localmente, ANTES de qualquer tentativa de escrever no socket, se
 * a GUI pode enviar esta mensagem. canais/n_canais só é usado para
 * LIA_MSG_ASSINAR; ignorado para os demais tipos. */
LiaEnvioErro lia_sessao_validar_envio(const LiaSessao *s, LiaMsgTipo tipo,
                                      const char * const *canais, size_t n_canais);

#endif
