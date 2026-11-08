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

echo
if [ "$falhas_totais" -eq 0 ]; then
    echo "teste_cliente_ollama: todos os modos passaram"
else
    echo "teste_cliente_ollama: $falhas_totais modo(s) com falha"
fi
exit "$falhas_totais"
