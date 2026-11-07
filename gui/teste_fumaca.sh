#!/bin/sh
# Teste de fumaça da GUI: requer display (X11/Wayland) e build/lia-gui já
# compilado (`make gui`). Verifica, nesta ordem:
#   1. nenhuma referência a sudo/pkexec/system/exec* nos binários da GUI;
#   2. nenhuma dependência dinâmica de libpolkit/libsudo;
#   3. a GUI sobe e continua viva por alguns segundos em modo sem servidor
#      (não há daemon em ~/.lia/maquina/estado/sessao.sock nesta etapa).
#
# Separado de `make teste-gui` (que só roda gui/teste_protocolo.c, sem
# display) de propósito: este script abre uma janela por ~2s. Uso manual ou
# em ambiente com display: `make gui-smoke`.
set -u
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
GUI="$RAIZ/build/lia-gui"
GUITESTE="$RAIZ/build/lia-gui-teste"
falhas=0; total=0
ok()  { total=$((total+1)); printf 'ok   %s\n' "$1"; }
ko()  { total=$((total+1)); falhas=$((falhas+1)); printf 'FALHA %s\n' "$1"; }

if [ ! -x "$GUI" ]; then
    echo "lia-gui não encontrado em $GUI — rode 'make gui' primeiro"
    exit 1
fi

for bin in "$GUI" "$GUITESTE"; do
    nome=$(basename "$bin")
    [ -x "$bin" ] || continue
    strings -a "$bin" 2>/dev/null | grep -qiE '^(sudo|pkexec|system|execve|execv|execvp|execl|execlp)$' \
        && ko "$nome: referência a sudo/pkexec/system/exec* encontrada" \
        || ok "$nome: nenhuma string de sudo/pkexec/system/exec*"
    ldd "$bin" 2>/dev/null | grep -qiE 'libpolkit|libsudo' \
        && ko "$nome: linka dinamicamente com polkit/sudo" \
        || ok "$nome: sem dependência dinâmica de polkit/sudo"
done

if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "sem DISPLAY/WAYLAND_DISPLAY — pulando o teste de execução real da janela"
else
    SOCK="$HOME/.lia/maquina/estado/sessao.sock"
    [ -e "$SOCK" ] && ko "sessao.sock existe nesta máquina — teste de 'sem servidor' não é representativo" \
        || ok "sem daemon em $SOCK (modo sem servidor é o estado real agora)"

    "$GUI" >/tmp/lia-gui-fumaca.out 2>&1 &
    pid=$!
    sleep 2
    if kill -0 "$pid" 2>/dev/null; then
        ok "lia-gui continua viva depois de 2s em modo sem servidor (não travou, não caiu)"
    else
        ko "lia-gui saiu sozinha em menos de 2s (deveria continuar rodando, só desconectada)"
    fi
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
fi

printf '\n%d testes, %d falhas\n' "$total" "$falhas"
[ "$falhas" -eq 0 ]
