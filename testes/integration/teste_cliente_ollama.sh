#!/bin/sh
# Testes unitários diretos do cliente Ollama e do caminho de status do
# backend de modelo (sessiond/modelo_ollama.c) — correções da auditoria
# pós-commit e366f2b: validação centralizada de configuração e
# ollama_checar_disponivel() nunca conectando fora de
# 127.0.0.1/localhost. Uso: make teste-ollama
set -u
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$RAIZ/build/teste-cliente-ollama"

if [ ! -x "$BIN" ]; then
    echo "build/teste-cliente-ollama não encontrado — rode 'make teste-ollama' (ele compila antes)"
    exit 1
fi

HOME_T=$(mktemp -d)
case "$HOME_T" in
    /tmp/*) : ;;
    *) echo "ERRO FATAL: HOME_T não está sob /tmp — abortando"; exit 1 ;;
esac
trap 'rm -rf "$HOME_T"' EXIT

falhas_totais=0

echo "== validação centralizada de configuração (host/porta/timeout/modelo) =="
env HOME="$HOME_T" "$BIN" config_validar
rc=$?
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== status (disponivel()) com host inválido: nunca tenta conexão de rede =="
env HOME="$HOME_T" LIA_OLLAMA_HOST=evil.example.com LIA_OLLAMA_PORTA=80 "$BIN" status_host_invalido
rc=$?
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== status (disponivel()) com host válido mas sem servidor: tenta e falha normalmente =="
env HOME="$HOME_T" LIA_OLLAMA_HOST=127.0.0.1 LIA_OLLAMA_PORTA=1 "$BIN" status_host_valido_sem_servidor
rc=$?
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== término da geração: servidor falso 'normal' -> completo =="
env OLLAMA_FAKE_MODO=normal python3 "$RAIZ/testes/integration/ollama_fake.py" 0 > "$HOME_T/fake.port" 2>/dev/null &
FPID=$!
i=0; while [ ! -s "$HOME_T/fake.port" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
env HOME="$HOME_T" LIA_OLLAMA_PORTA="$(head -1 "$HOME_T/fake.port" | tr -dc '0-9')" "$BIN" fim completo 2
rc=$?
kill "$FPID" 2>/dev/null; wait "$FPID" 2>/dev/null; rm -f "$HOME_T/fake.port"
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== término da geração: servidor falso 'limite' -> limite_tokens =="
env OLLAMA_FAKE_MODO=limite python3 "$RAIZ/testes/integration/ollama_fake.py" 0 > "$HOME_T/fake.port" 2>/dev/null &
FPID=$!
i=0; while [ ! -s "$HOME_T/fake.port" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
env HOME="$HOME_T" LIA_OLLAMA_PORTA="$(head -1 "$HOME_T/fake.port" | tr -dc '0-9')" "$BIN" fim limite_tokens 7
rc=$?
kill "$FPID" 2>/dev/null; wait "$FPID" 2>/dev/null; rm -f "$HOME_T/fake.port"
[ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))

echo "== opções: num_ctx 8192 e temperatura 0 por padrão; semente/temperatura só quando configuradas =="
for caso in "padrao" "avaliacao"; do
    env OLLAMA_FAKE_MODO=ecoar_opcoes python3 "$RAIZ/testes/integration/ollama_fake.py" 0 > "$HOME_T/fake.port" 2>/dev/null &
    FPID=$!
    i=0; while [ ! -s "$HOME_T/fake.port" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
    P=$(head -1 "$HOME_T/fake.port" | tr -dc '0-9')
    if [ "$caso" = padrao ]; then
        env HOME="$HOME_T" LIA_OLLAMA_PORTA="$P" "$BIN" opcoes '"num_ctx": 8192' '"temperature": 0'
    else
        env HOME="$HOME_T" LIA_OLLAMA_PORTA="$P" LIA_MODELO_TEMPERATURA=0.4 LIA_MODELO_SEMENTE=3 LIA_OLLAMA_NUM_CTX=12288 "$BIN" opcoes '"num_ctx": 12288' '"seed": 3' '"temperature": 0.4'
    fi
    rc=$?
    kill "$FPID" 2>/dev/null; wait "$FPID" 2>/dev/null; rm -f "$HOME_T/fake.port"
    [ "$rc" -ne 0 ] && falhas_totais=$((falhas_totais + 1))
done

echo
if [ "$falhas_totais" -eq 0 ]; then
    echo "teste_cliente_ollama: todos os modos passaram"
else
    echo "teste_cliente_ollama: $falhas_totais modo(s) com falha"
fi
exit "$falhas_totais"
