#!/bin/sh
# chat/rodar_local.sh — inicia lia-sessiond num $HOME de teste e abre
# lia-chat, tudo num só terminal. Para testes manuais rápidos com
# prompts reais — NUNCA usa o $HOME real da pessoa.
#
# Uso: sh chat/rodar_local.sh [TEST_HOME] [mock|real] [mock|ollama|llama_cpp]
#   TEST_HOME padrão: /tmp/lia-chat-teste (reaproveitado entre execuções,
#   para manter o estado de ~/.lia de teste entre uma sessão e outra —
#   apague o diretório manualmente se quiser começar do zero).
#   executor padrão: "real" (as seis ferramentas de leitura de verdade —
#   tools.discover/list/probe, sistema.diagnosticar/recursos/dispositivos
#   — mesmo padrão de produção).
#   modelo: se omitido, é AUTO-DETECTADO (ver abaixo) — nunca assume
#   silenciosamente que Ollama/llama.cpp estão rodando; se nenhum dos
#   dois for encontrado, cai para mock e explica por quê.
#
# Detecção de modelos LLM locais (antes de iniciar qualquer coisa):
#   - Ollama: tenta http://$LIA_OLLAMA_HOST:$LIA_OLLAMA_PORTA/api/tags
#     (padrão 127.0.0.1:11434) e confere se há ao menos um modelo baixado.
#   - llama.cpp (llama-server): tenta
#     http://$LIA_LLAMACPP_HOST:$LIA_LLAMACPP_PORTA/health (padrão
#     127.0.0.1:8080) — um llama-server só serve o modelo que foi
#     carregado na linha de comando dele; este script nunca inicia um
#     llama-server sozinho (não sabe qual .gguf nem com que flags você
#     quer rodar), só detecta se um já está no ar e usa.
#   - se nenhum dos dois estiver alcançável, procura arquivos .gguf em
#     ~/models (só para avisar e sugerir o comando de iniciar o
#     llama-server, nunca para rodar nada automaticamente).
#
# Dentro do lia-chat: /help, /status, /modelo, MOCK_PROPOR:<ferramenta>
# (só faz sentido com modelo=mock), /plano, /aprovar, /rejeitar,
# /cancelar, /sair. Ctrl-C no lia-chat também funciona (cancela o que
# estiver pendente; duas vezes em menos de 2s força saída). Encerrar o
# lia-chat (/sair) já encerra o daemon sozinho.
set -u
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
TEST_HOME="${1:-${TEST_HOME:-/tmp/lia-chat-teste}}"
EXECUTOR="${2:-${EXECUTOR:-real}}"
MODELO_PEDIDO="${3:-${MODELO:-auto}}"
case "$EXECUTOR" in
    mock|real) : ;;
    *) echo "uso: sh chat/rodar_local.sh [TEST_HOME] [mock|real] [mock|ollama|llama_cpp]" >&2; exit 2 ;;
esac
case "$MODELO_PEDIDO" in
    auto|mock|ollama|llama_cpp) : ;;
    *) echo "uso: sh chat/rodar_local.sh [TEST_HOME] [mock|real] [mock|ollama|llama_cpp]" >&2; exit 2 ;;
esac

case "$TEST_HOME" in
    /tmp/*) : ;;
    *) echo "recusando: TEST_HOME ('$TEST_HOME') precisa estar sob /tmp" >&2; exit 1 ;;
esac
if [ "$TEST_HOME" = "$HOME" ]; then
    echo "recusando: TEST_HOME não pode ser o HOME real" >&2
    exit 1
fi

OLLAMA_HOST="${LIA_OLLAMA_HOST:-127.0.0.1}"
OLLAMA_PORTA="${LIA_OLLAMA_PORTA:-11434}"
LLAMACPP_HOST="${LIA_LLAMACPP_HOST:-127.0.0.1}"
LLAMACPP_PORTA="${LIA_LLAMACPP_PORTA:-8080}"

# detectar_ollama: 0 se http://$OLLAMA_HOST:$OLLAMA_PORTA tiver ao menos
# um modelo baixado (/api/tags), 1 senão. Nunca assume: se 'curl' não
# existir, trata como "não detectado" (nunca como "detectado"). Também
# guarda em OLLAMA_MODELO_DETECTADO o nome do primeiro modelo baixado —
# sem isso, o daemon cairia no padrão hardcoded de
# ollama/cliente_ollama.c ("llama3"), que quase nunca é o que está
# realmente baixado (ex.: "llama3.2:3b"), e toda pergunta falharia com
# "Ollama respondeu com erro HTTP" (model not found) sem nenhuma pista
# de por quê.
detectar_ollama() {
    command -v curl >/dev/null 2>&1 || return 1
    resposta=$(curl -s --max-time 2 "http://$OLLAMA_HOST:$OLLAMA_PORTA/api/tags" 2>/dev/null) || return 1
    case "$resposta" in
        *'"models":[]'*) return 1 ;;  # Ollama no ar, mas sem nenhum modelo baixado
        *'"models":'*)
            OLLAMA_MODELO_DETECTADO=$(printf '%s' "$resposta" | sed -n 's/.*"name":"\([^"]*\)".*/\1/p' | head -1)
            return 0
            ;;
        *) return 1 ;;
    esac
}

# detectar_llama_cpp: 0 se http://$LLAMACPP_HOST:$LLAMACPP_PORTA/health
# responder {"status":"ok"} (modelo já carregado e pronto).
detectar_llama_cpp() {
    command -v curl >/dev/null 2>&1 || return 1
    resposta=$(curl -s --max-time 2 "http://$LLAMACPP_HOST:$LLAMACPP_PORTA/health" 2>/dev/null) || return 1
    case "$resposta" in
        *'"status":"ok"'*) return 0 ;;
        *) return 1 ;;
    esac
}

echo "detectando modelos LLM locais..."
OLLAMA_MODELO_DETECTADO=""
OLLAMA_OK=1; detectar_ollama && OLLAMA_OK=0
LLAMACPP_OK=1; detectar_llama_cpp && LLAMACPP_OK=0

if [ "$OLLAMA_OK" -eq 0 ]; then
    echo "  Ollama: alcançável em $OLLAMA_HOST:$OLLAMA_PORTA, modelo '$OLLAMA_MODELO_DETECTADO' baixado"
else
    echo "  Ollama: não encontrado em $OLLAMA_HOST:$OLLAMA_PORTA (ou sem nenhum modelo baixado)"
fi
if [ "$LLAMACPP_OK" -eq 0 ]; then
    echo "  llama.cpp (llama-server): alcançável em $LLAMACPP_HOST:$LLAMACPP_PORTA, modelo carregado"
else
    echo "  llama.cpp (llama-server): não encontrado em $LLAMACPP_HOST:$LLAMACPP_PORTA"
fi

if [ "$OLLAMA_OK" -ne 0 ] && [ "$LLAMACPP_OK" -ne 0 ]; then
    GGUF_ACHADOS=$(find "$HOME/models" -maxdepth 2 -iname '*.gguf' 2>/dev/null | head -5)
    if [ -n "$GGUF_ACHADOS" ]; then
        echo "  arquivo(s) .gguf encontrado(s) em $HOME/models, mas nenhum llama-server rodando com eles:"
        echo "$GGUF_ACHADOS" | sed 's/^/    /'
        primeiro=$(printf '%s\n' "$GGUF_ACHADOS" | head -1)
        echo "  para usar um deles, rode antes (noutro terminal):"
        echo "    llama-server -m '$primeiro' --host $LLAMACPP_HOST --port $LLAMACPP_PORTA -ngl 0"
    fi
fi

case "$MODELO_PEDIDO" in
    auto)
        if [ "$LLAMACPP_OK" -eq 0 ]; then MODELO=llama_cpp
        elif [ "$OLLAMA_OK" -eq 0 ]; then MODELO=ollama
        else MODELO=mock
        fi
        echo "modelo (auto-detectado): $MODELO"
        ;;
    mock|ollama|llama_cpp)
        MODELO="$MODELO_PEDIDO"
        echo "modelo (pedido explicitamente): $MODELO"
        if [ "$MODELO" = ollama ] && [ "$OLLAMA_OK" -ne 0 ]; then
            echo "  aviso: Ollama não parece alcançável agora — /modelo vai reportar indisponível, mas o chat funciona normalmente"
        fi
        if [ "$MODELO" = llama_cpp ] && [ "$LLAMACPP_OK" -ne 0 ]; then
            echo "  aviso: llama-server não parece alcançável agora — /modelo vai reportar indisponível, mas o chat funciona normalmente"
        fi
        ;;
esac

mkdir -p "$TEST_HOME"
SOCK="$TEST_HOME/.lia/run/session.sock"

echo "compilando (make chat sessiond)..."
(cd "$RAIZ" && make chat sessiond) || exit 1

echo "HOME de teste: $TEST_HOME (executor=$EXECUTOR, modelo=$MODELO, log em $TEST_HOME/sessiond.log)"
ENV_EXECUTOR=""
[ "$EXECUTOR" = mock ] && ENV_EXECUTOR="LIA_SESSIOND_EXECUTOR=mock"
ENV_MODELO=""
[ "$MODELO" != mock ] && ENV_MODELO="LIA_SESSIOND_MODELO=$MODELO"
ENV_OLLAMA_MODELO=""
if [ "$MODELO" = ollama ] && [ -z "${LIA_OLLAMA_MODELO:-}" ] && [ -n "$OLLAMA_MODELO_DETECTADO" ]; then
    ENV_OLLAMA_MODELO="LIA_OLLAMA_MODELO=$OLLAMA_MODELO_DETECTADO"
    echo "  usando o modelo Ollama detectado: $OLLAMA_MODELO_DETECTADO (defina LIA_OLLAMA_MODELO para escolher outro)"
fi

env HOME="$TEST_HOME" $ENV_EXECUTOR $ENV_MODELO $ENV_OLLAMA_MODELO "$RAIZ/build/lia-sessiond" > "$TEST_HOME/sessiond.log" 2>&1 &
DAEMON_PID=$!
trap 'kill "$DAEMON_PID" 2>/dev/null; wait "$DAEMON_PID" 2>/dev/null' EXIT INT TERM

i=0
while [ ! -S "$SOCK" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
if [ ! -S "$SOCK" ]; then
    echo "daemon não respondeu a tempo — veja $TEST_HOME/sessiond.log" >&2
    exit 1
fi

echo "conectando lia-chat (/help para comandos, /sair para encerrar)..."
echo
env HOME="$TEST_HOME" "$RAIZ/build/lia-chat" --backend "$MODELO"
