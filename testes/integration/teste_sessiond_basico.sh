#!/bin/sh
# Teste básico do daemon isolado (sem GUI, sem múltiplos clientes, sem
# socket stale) — `make teste-sessiond`. O teste completo com todos os
# cenários de docs/protocolo.md está em teste_integracao.sh (`make
# integration`). Nunca toca ~/.lia da pessoa: roda em $HOME temporário.
set -u
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
SESSIOND="$RAIZ/build/lia-sessiond"

if [ ! -x "$SESSIOND" ]; then
    echo "lia-sessiond não encontrado em $SESSIOND — rode 'make sessiond' primeiro"
    exit 1
fi
if ! command -v nc >/dev/null; then
    echo "'nc' (netcat com -U) não encontrado"
    exit 1
fi

falhas=0; total=0
ok()  { total=$((total+1)); printf 'ok   %s\n' "$1"; }
ko()  { total=$((total+1)); falhas=$((falhas+1)); printf 'FALHA %s\n' "$1"; }

HOME_T=$(mktemp -d)
SOCK="$HOME_T/.lia/run/session.sock"
trap 'rm -rf "$HOME_T"' EXIT

env HOME="$HOME_T" "$SESSIOND" > "$HOME_T/sessiond.log" 2>&1 &
PID=$!
i=0
while [ ! -S "$SOCK" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done

[ -S "$SOCK" ] && ok "lia-sessiond cria o socket em ~/.lia/run/session.sock" || ko "socket não criado"
[ "$(stat -c %a "$HOME_T/.lia/run" 2>/dev/null)" = "700" ] && ok "diretório run/ com modo 700" || ko "modo do diretório"
[ "$(stat -c %a "$SOCK" 2>/dev/null)" = "600" ] && ok "socket com modo 600" || ko "modo do socket"

grep -q "^lia-sessiond: escutando" "$HOME_T/sessiond.log" && ok "banner de inicialização no log" || ko "sem banner de inicialização"

SAIDA=$(mktemp)
printf '{"protocolo":"lia.v1","tipo":"solicitar","id":"b1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"sistema.diagnosticar","parametros":{}}}\n' \
    | timeout 5 nc -U "$SOCK" > "$SAIDA"
grep -q '"tipo":"plano"' "$SAIDA" && ok "roundtrip básico (executor real, sistema.diagnosticar): plano recebido" || ko "sem plano"
grep -q '"estado":"sucesso"' "$SAIDA" && ok "roundtrip básico: resultado sucesso, dados reais da máquina" || ko "sem resultado de sucesso"
grep -q '"distro"' "$SAIDA" && ok "resultado contém dados reais (distro) — não é mock" || ko "resultado não contém dados reais"
rm -f "$SAIDA"

kill -TERM "$PID" 2>/dev/null
i=0
while kill -0 "$PID" 2>/dev/null && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i+1)); done
kill -0 "$PID" 2>/dev/null && ko "daemon não encerrou a tempo com SIGTERM" || ok "daemon encerra com SIGTERM"
[ -S "$SOCK" ] && ko "socket não removido ao encerrar" || ok "socket removido ao encerrar"

# não roda como root
if sudo -n true >/dev/null 2>&1; then
    HOME_R=$(mktemp -d)
    sudo -n env HOME="$HOME_R" "$SESSIOND" >"$HOME_R/saida.log" 2>&1 &
    RPID=$!
    sleep 0.3
    if kill -0 "$RPID" 2>/dev/null; then
        ko "lia-sessiond continuou rodando como root (deveria recusar)"
        sudo -n kill "$RPID" 2>/dev/null
    else
        grep -qi "recusa executar como root" "$HOME_R/saida.log" && ok "lia-sessiond recusa executar como root" \
            || ko "lia-sessiond saiu ao rodar como root, mas sem a mensagem esperada"
    fi
    rm -rf "$HOME_R"
else
    grep -qi "recusa executar como root" "$RAIZ/sessiond/sessiond.c" \
        && echo "(sem sudo -n: checagem de root confirmada só no código-fonte, não em runtime)" \
        || ko "checagem de root não encontrada no código"
fi

printf '\n%d testes, %d falhas\n' "$total" "$falhas"
[ "$falhas" -eq 0 ]
