#!/bin/sh
# Testes unitários diretos do cliente llama.cpp e do caminho de status do
# backend de modelo (sessiond/modelo_llama_cpp.c) — espelha
# testes/integration/teste_cliente_ollama.sh. Uso: make teste-llama-cpp
set -u
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$RAIZ/build/teste-cliente-llama-cpp"

if [ ! -x "$BIN" ]; then
    echo "build/teste-cliente-llama-cpp não encontrado — rode 'make teste-llama-cpp' (ele compila antes)"
    exit 1
fi

HOME_T=$(mktemp -d)
case "$HOME_T" in
    /tmp/*) : ;;
    *) echo "ERRO FATAL: HOME_T não está sob /tmp — abortando"; exit 1 ;;
esac
trap 'rm -rf "$HOME_T"' EXIT

falhas_totais=0

echo "== validação centralizada de configuração (host/porta/timeout) =="
env HOME="$HOME_T" "$BIN" config_validar
rc=$?
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== status (disponivel()) com host inválido: nunca tenta conexão de rede =="
env HOME="$HOME_T" LIA_LLAMACPP_HOST=evil.example.com LIA_LLAMACPP_PORTA=80 "$BIN" status_host_invalido
rc=$?
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== status (disponivel()) com host válido mas sem servidor: tenta e falha normalmente =="
env HOME="$HOME_T" LIA_LLAMACPP_HOST=127.0.0.1 LIA_LLAMACPP_PORTA=1 "$BIN" status_host_valido_sem_servidor
rc=$?
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== término da geração: servidor falso 'normal' -> completo =="
env LLAMACPP_FAKE_MODO=normal python3 "$RAIZ/testes/integration/llama_cpp_fake.py" 0 > "$HOME_T/fake.port" 2>/dev/null &
FPID=$!
i=0; while [ ! -s "$HOME_T/fake.port" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
env HOME="$HOME_T" LIA_LLAMACPP_PORTA="$(head -1 "$HOME_T/fake.port" | tr -dc '0-9')" "$BIN" fim completo 2
rc=$?
kill "$FPID" 2>/dev/null; wait "$FPID" 2>/dev/null; rm -f "$HOME_T/fake.port"
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== término da geração: servidor falso 'limite' -> limite_tokens =="
env LLAMACPP_FAKE_MODO=limite python3 "$RAIZ/testes/integration/llama_cpp_fake.py" 0 > "$HOME_T/fake.port" 2>/dev/null &
FPID=$!
i=0; while [ ! -s "$HOME_T/fake.port" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
env HOME="$HOME_T" LIA_LLAMACPP_PORTA="$(head -1 "$HOME_T/fake.port" | tr -dc '0-9')" "$BIN" fim limite_tokens 7
rc=$?
kill "$FPID" 2>/dev/null; wait "$FPID" 2>/dev/null; rm -f "$HOME_T/fake.port"
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== término da geração: servidor falso 'limite_antigo' -> limite_tokens =="
env LLAMACPP_FAKE_MODO=limite_antigo python3 "$RAIZ/testes/integration/llama_cpp_fake.py" 0 > "$HOME_T/fake.port" 2>/dev/null &
FPID=$!
i=0; while [ ! -s "$HOME_T/fake.port" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
env HOME="$HOME_T" LIA_LLAMACPP_PORTA="$(head -1 "$HOME_T/fake.port" | tr -dc '0-9')" "$BIN" fim limite_tokens 7
rc=$?
kill "$FPID" 2>/dev/null; wait "$FPID" 2>/dev/null; rm -f "$HOME_T/fake.port"
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo
if [ "$falhas_totais" -eq 0 ]; then
    echo "teste_cliente_llama_cpp: todos os modos passaram"
else
    echo "teste_cliente_llama_cpp: $falhas_totais modo(s) com falha"
fi
exit "$falhas_totais"
