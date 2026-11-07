/* Lia — cliente HTTP mínimo para o Ollama (`/api/generate`), hospedado em
 * C (libc + GLib/GIO — fora do núcleo sem libc). Decodifica HTTP/1.1
 * (status, cabeçalhos, `Transfer-Encoding: chunked` ou `Content-Length`)
 * e NDJSON de streaming manualmente: sem libcurl, sem SDK, "mínimo" no
 * sentido literal.
 *
 * Restrição de segurança, não apenas padrão: o host só pode ser
 * `127.0.0.1` ou `localhost` — qualquer outro valor é rejeitado antes de
 * qualquer E/S (docs/THREAT-MODEL.md, "nunca enviar dados para fora de
 * 127.0.0.1"). Não há bypass nem flag para desligar isto nesta etapa.
 */
#ifndef LIA_OLLAMA_CLIENTE_H
#define LIA_OLLAMA_CLIENTE_H

#include <glib.h>

typedef struct {
    char *host;                 /* sempre "127.0.0.1" ou "localhost" após validação */
    int porta;
    char *modelo;
    int timeout_ms;              /* timeout de conexão e de inatividade entre leituras */
    gsize limite_resposta_bytes; /* aborta com OLLAMA_ERRO_LIMITE_EXCEDIDO se exceder */
} OllamaConfig;

typedef enum {
    OLLAMA_OK = 0,
    OLLAMA_ERRO_ENDPOINT_INVALIDO,  /* host != 127.0.0.1/localhost, ou URL malformada */
    OLLAMA_ERRO_CONEXAO,            /* Ollama indisponível: connection refused, etc. */
    OLLAMA_ERRO_TIMEOUT,
    OLLAMA_ERRO_LIMITE_EXCEDIDO,
    OLLAMA_ERRO_RESPOSTA_INVALIDA,  /* HTTP malformado, JSON malformado, fechou sem 'done' */
    OLLAMA_ERRO_CANCELADO,
    OLLAMA_ERRO_HTTP,               /* status HTTP != 200 */
} OllamaErro;

const char *ollama_erro_texto(OllamaErro e);

/* host_ou_null: NULL/"" usa "127.0.0.1". Qualquer outro valor que não seja
 * exatamente "127.0.0.1" ou "localhost" é aceito aqui (o config é só
 * dados), mas ollama_gerar() recusa a requisição com
 * OLLAMA_ERRO_ENDPOINT_INVALIDO antes de abrir qualquer conexão. */
OllamaConfig *ollama_config_novo(const char *host_ou_null, int porta, const char *modelo);
void ollama_config_liberar(OllamaConfig *cfg);

typedef struct OllamaRequisicao OllamaRequisicao;

/* Chamada exatamente uma vez. texto_completo só é não-NULL quando
 * erro == OLLAMA_OK; nesse caso é uma referência válida apenas durante a
 * chamada (copie se precisar manter). Depois desta chamada, 'req' (se não
 * NULL) já foi liberado — nunca chame ollama_cancelar com ele depois. */
typedef void (*OllamaConcluidoCb)(gpointer ctx, OllamaErro erro, const char *texto_completo);

/* POST /api/generate {model, prompt, stream:true}. 'prompt' já deve vir
 * pronto (ver sessiond/prompt.h) — este módulo não sabe nada sobre
 * confiança de conteúdo, só transporta bytes.
 *
 * Validação de endpoint falha antes de qualquer E/S: concluido_cb ainda é
 * chamada (com OLLAMA_ERRO_ENDPOINT_INVALIDO), de forma assíncrona (via
 * g_idle), para o contrato ser sempre "uma chamada, sempre assíncrona" —
 * e a função retorna NULL nesse caso (nada para cancelar). */
OllamaRequisicao *ollama_gerar(OllamaConfig *cfg, const char *prompt,
                                OllamaConcluidoCb concluido_cb, gpointer ctx);

/* Cancelamento cooperativo: fecha a conexão e garante que concluido_cb
 * seja chamada (OLLAMA_ERRO_CANCELADO) se ainda não tiver sido. Nunca
 * chame depois que concluido_cb já tiver sido chamada para esta req. */
void ollama_cancelar(OllamaRequisicao *req);

#endif
