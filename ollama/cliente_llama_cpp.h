/* Lia — cliente HTTP mínimo para o llama.cpp `llama-server` nativo
 * (`/completion`), hospedado em C (libc + GLib/GIO), espelhando
 * ollama/cliente_ollama.h. Decodifica HTTP/1.1 (igual ao cliente Ollama)
 * e o streaming de eventos Server-Sent Events do llama-server
 * ("data: {...}\n\n" por evento) manualmente: sem libcurl, sem SDK.
 *
 * Diferença de framing em relação ao Ollama: o corpo não é NDJSON
 * (um objeto por linha) — é SSE (um objeto por "evento", terminado por
 * linha vazia, com prefixo "data:"). Por isso é um cliente separado, não
 * uma reconfiguração do cliente_ollama.c, apesar de reaproveitar a mesma
 * estrutura de conexão/cabeçalhos/chunked/timeout/cancelamento.
 *
 * Mesma restrição de segurança do cliente Ollama, não apenas padrão: o
 * host só pode ser `127.0.0.1` ou `localhost` (docs/THREAT-MODEL.md).
 *
 * Sem campo "modelo": o llama-server serve um único modelo por processo
 * (o caminho do .gguf passado na linha de comando dele) — não há seleção
 * de modelo por requisição como no Ollama.
 */
#ifndef LIA_LLAMA_CPP_CLIENTE_H
#define LIA_LLAMA_CPP_CLIENTE_H

#include <glib.h>

typedef struct {
    char *host;                 /* sempre "127.0.0.1" ou "localhost" após validação */
    int porta;
    int timeout_ms;              /* timeout de conexão e de inatividade entre leituras */
    gsize limite_resposta_bytes; /* aborta com LLAMACPP_ERRO_LIMITE_EXCEDIDO se exceder */
} LlamaCppConfig;

#define LLAMACPP_PORTA_MIN        1
#define LLAMACPP_PORTA_MAX        65535
#define LLAMACPP_TIMEOUT_MS_MIN   1
#define LLAMACPP_TIMEOUT_MS_MAX   (600 * 1000) /* 10 minutos — generoso, mas sempre finito */

typedef enum {
    LLAMACPP_OK = 0,
    LLAMACPP_ERRO_ENDPOINT_INVALIDO, /* host != 127.0.0.1/localhost, ou URL malformada */
    LLAMACPP_ERRO_CONEXAO,           /* llama-server indisponível: connection refused, etc. */
    LLAMACPP_ERRO_TIMEOUT,
    LLAMACPP_ERRO_LIMITE_EXCEDIDO,
    LLAMACPP_ERRO_RESPOSTA_INVALIDA, /* HTTP/SSE malformado, ou fechou sem evento final */
    LLAMACPP_ERRO_CANCELADO,
    LLAMACPP_ERRO_HTTP,              /* status HTTP != 200, ou evento SSE com "error" embutido */
} LlamaCppErro;

const char *llama_cpp_erro_texto(LlamaCppErro e);

gboolean llama_cpp_host_e_loopback(const char *host);

/* Validação centralizada de host/porta/timeout — mesma política do
 * cliente Ollama (ollama/cliente_ollama.h): a ÚNICA função que qualquer
 * chamador deve consultar antes de qualquer E/S de rede. */
char *llama_cpp_config_validar(const LlamaCppConfig *cfg);

LlamaCppConfig *llama_cpp_config_novo(const char *host_ou_null, int porta);
void llama_cpp_config_liberar(LlamaCppConfig *cfg);

typedef struct LlamaCppRequisicao LlamaCppRequisicao;

/* Chamada exatamente uma vez. texto_completo só é não-NULL quando
 * erro == LLAMACPP_OK; referência válida apenas durante a chamada. Depois
 * desta chamada, 'req' (se não NULL) já foi liberado. */
typedef void (*LlamaCppConcluidoCb)(gpointer ctx, LlamaCppErro erro, const char *texto_completo);

/* POST /completion {prompt, stream:true} — o endpoint nativo do
 * llama-server, sem formatação de chat/roles (o prompt já vem pronto de
 * sessiond/prompt.c, igual ao Ollama via /api/generate). */
LlamaCppRequisicao *llama_cpp_gerar(LlamaCppConfig *cfg, const char *prompt,
                                     LlamaCppConcluidoCb concluido_cb, gpointer ctx);

void llama_cpp_cancelar(LlamaCppRequisicao *req);

#endif
