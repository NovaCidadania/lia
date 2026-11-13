#!/bin/sh
# chat/rodar_local.sh — inicia lia-sessiond (modelo mock, sem Ollama) num
# $HOME de teste e abre lia-chat, tudo num só terminal. Para testes
# manuais rápidos com prompts reais — NUNCA usa o $HOME real da pessoa.
#
# Uso: sh chat/rodar_local.sh [TEST_HOME] [mock|real]
#   TEST_HOME padrão: /tmp/lia-chat-teste (reaproveitado entre execuções,
#   para manter o estado de ~/.lia de teste entre uma sessão e outra —
#   apague o diretório manualmente se quiser começar do zero).
#   executor padrão: "real" (as seis ferramentas de leitura de verdade —
#   tools.discover/list/probe, sistema.diagnosticar/recursos/dispositivos
#   — mesmo padrão de produção; o MODELO continua mock, então não precisa
#   de Ollama). Passe "mock" para testar a máquina de estados do
#   protocolo em si com mock.leitura/mock.escrita/mock.admin/mock.timeout
#   (nomes que só existem nesse executor fictício).
#
# Dentro do lia-chat: /help, /status, MOCK_PROPOR:<ferramenta>, /plano,
# /aprovar, /rejeitar, /cancelar, /sair. Ctrl-C no lia-chat também
# funciona (cancela o que estiver pendente; duas vezes em menos de 2s
# força saída). Encerrar o lia-chat (/sair) já encerra o daemon sozinho.
set -u
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
TEST_HOME="${1:-${TEST_HOME:-/tmp/lia-chat-teste}}"
EXECUTOR="${2:-${EXECUTOR:-real}}"
case "$EXECUTOR" in
    mock|real) : ;;
    *) echo "uso: sh chat/rodar_local.sh [TEST_HOME] [mock|real]" >&2; exit 2 ;;
esac

case "$TEST_HOME" in
    /tmp/*) : ;;
    *) echo "recusando: TEST_HOME ('$TEST_HOME') precisa estar sob /tmp" >&2; exit 1 ;;
esac
if [ "$TEST_HOME" = "$HOME" ]; then
    echo "recusando: TEST_HOME não pode ser o HOME real" >&2
    exit 1
fi

mkdir -p "$TEST_HOME"
SOCK="$TEST_HOME/.lia/run/session.sock"

echo "compilando (make chat sessiond)..."
(cd "$RAIZ" && make chat sessiond) || exit 1

echo "HOME de teste: $TEST_HOME (executor=$EXECUTOR, modelo=mock, sem Ollama, log em $TEST_HOME/sessiond.log)"
if [ "$EXECUTOR" = mock ]; then
    env HOME="$TEST_HOME" LIA_SESSIOND_EXECUTOR=mock "$RAIZ/build/lia-sessiond" > "$TEST_HOME/sessiond.log" 2>&1 &
else
    env HOME="$TEST_HOME" "$RAIZ/build/lia-sessiond" > "$TEST_HOME/sessiond.log" 2>&1 &
fi
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
env HOME="$TEST_HOME" "$RAIZ/build/lia-chat"
