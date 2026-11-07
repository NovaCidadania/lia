#!/bin/sh
# Testes de integração do daemon de sessão (sessiond/sessiond.c) contra o
# protocolo real, em socket Unix de verdade, em um $HOME temporário —
# nunca toca ~/.lia da pessoa. Uso: make integration
set -u
trap '' PIPE
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
SESSIOND="$RAIZ/build/lia-sessiond"
GUI="$RAIZ/build/lia-gui"

if [ ! -x "$SESSIOND" ]; then
    echo "lia-sessiond não encontrado em $SESSIOND — rode 'make sessiond' primeiro"
    exit 1
fi
if ! command -v nc >/dev/null; then
    echo "'nc' (netcat com suporte a -U) não encontrado — necessário para estes testes"
    exit 1
fi

falhas=0; total=0
ok()  { total=$((total+1)); printf 'ok   %s\n' "$1"; }
ko()  { total=$((total+1)); falhas=$((falhas+1)); printf 'FALHA %s\n' "$1"; }

HOME_T=$(mktemp -d)
SOCK="$HOME_T/.lia/run/session.sock"
trap 'limpar_tudo' EXIT

DAEMON_PID=""
limpar_tudo() {
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null
    pkill -f "nc -U $SOCK" 2>/dev/null
    wait 2>/dev/null
    if [ -z "${LIA_DEBUG_MANTER:-}" ]; then
        rm -rf "$HOME_T"
    else
        echo "DEBUG: HOME_T preservado em $HOME_T"
    fi
}

iniciar_daemon() {
    env HOME="$HOME_T" "$SESSIOND" > "$HOME_T/sessiond.log" 2>&1 &
    DAEMON_PID=$!
    i=0
    while [ ! -S "$SOCK" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
}

parar_daemon() {
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=""
}

# abre uma conexão persistente via FIFO + nc -U; $1 = nome (para arquivos)
abrir_conexao() {
    nome=$1
    rm -f "$HOME_T/$nome.in"
    mkfifo "$HOME_T/$nome.in"
    : > "$HOME_T/$nome.out"
    nc -U "$SOCK" < "$HOME_T/$nome.in" > "$HOME_T/$nome.out" &
    eval "NC_PID_$nome=$!"
    eval "exec $([ "$nome" = a ] && echo 3 || echo 4)> '$HOME_T/$nome.in'"
}

enviar_a() { printf '%s\n' "$1" >&3; }
enviar_b() { printf '%s\n' "$1" >&4; }

fechar_a() { exec 3>&- 2>/dev/null; eval 'kill "$NC_PID_a"' 2>/dev/null; }
fechar_b() { exec 4>&- 2>/dev/null; eval 'kill "$NC_PID_b"' 2>/dev/null; }

aguardar() { # arquivo padrao [tentativas]
    arq=$1; pad=$2; tent=${3:-30}
    i=0
    while [ "$i" -lt "$tent" ]; do
        grep -q "$pad" "$arq" 2>/dev/null && return 0
        sleep 0.1
        i=$((i+1))
    done
    return 1
}

extrair_plano_id() { grep -o '"plano_id":"[^"]*"' "$1" | head -1 | cut -d'"' -f4; }

# ========================================================================
echo "== 1. daemon inicia em diretório temporário, socket correto =="
iniciar_daemon
[ -S "$SOCK" ] && ok "socket criado em \$HOME/.lia/run/session.sock" || ko "socket não foi criado"
[ "$(stat -c %a "$HOME_T/.lia/run" 2>/dev/null)" = "700" ] && ok "diretório run/ com modo 700" || ko "modo do diretório run/"
[ "$(stat -c %a "$SOCK" 2>/dev/null)" = "600" ] && ok "socket com modo 600" || ko "modo do socket"
[ "$(stat -c %U "$SOCK" 2>/dev/null)" = "$(id -un)" ] && ok "socket pertence ao usuário atual" || ko "dono do socket"

echo "== 2. cliente CLI de teste (NDJSON bruto) — fluxo nenhum (auto-executa) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"cli-1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.leitura","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"resultado"' && ok "cliente CLI: solicitar risco=nenhum -> plano+resultado sem aprovação" \
    || ko "cliente CLI: fluxo nenhum não completou"
aguardar "$HOME_T/a.out" '"tipo":"auditoria"' && ok "cliente CLI: auditoria emitida após o resultado" || ko "cliente CLI: sem auditoria"
fechar_a

echo "== 3. cliente GUI/protocolo (gui/protocolo.c real, via build/lia-gui se houver display) =="
if [ -x "$GUI" ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    env HOME="$HOME_T" "$GUI" > "$HOME_T/gui.log" 2>&1 &
    GUI_PID=$!
    sleep 1.5
    if kill -0 "$GUI_PID" 2>/dev/null; then
        grep -q "conectado ao daemon" "$HOME_T/gui.log" && ok "GUI conectou ao daemon real" || ko "GUI não registrou conexão"
        kill "$GUI_PID" 2>/dev/null; wait "$GUI_PID" 2>/dev/null
    else
        ko "GUI encerrou sozinha ao tentar conectar"
    fi
else
    echo "(sem build/lia-gui ou sem display — pulando o teste de GUI real; ver make gui-smoke)"
fi

echo "== 4. fluxo plano -> aprovação -> simulação -> resultado (risco médio) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"f1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.escrita","parametros":{"x":1}}}'
aguardar "$HOME_T/a.out" '"tipo":"plano"' && ok "plano emitido para ferramenta de risco médio" || ko "sem plano"
aguardar "$HOME_T/a.out" '"requer_aprovacao":true' && ok "plano marca requer_aprovacao=true" || ko "requer_aprovacao incorreto"
PID_PLANO=$(extrair_plano_id "$HOME_T/a.out")
enviar_a "$(printf '{"protocolo":"lia.v1","tipo":"aprovar","id":"f2","em_resposta_a":null,"ts":"t","dados":{"plano_id":"%s","decisao":"aprovar","escopo":"uma_vez"}}' "$PID_PLANO")"
aguardar "$HOME_T/a.out" '"tipo":"progresso"' && ok "progresso emitido após aprovação" || ko "sem progresso"
aguardar "$HOME_T/a.out" '"estado":"sucesso"' && ok "resultado sucesso após simulação completar" || ko "sem resultado de sucesso"
aguardar "$HOME_T/a.out" '"tipo":"auditoria"' && ok "auditoria emitida (fluxo com aprovação)" || ko "sem auditoria"
grep -q "acao=sessiond.mock.escrita" "$HOME_T/.lia/humano/auditoria"/*.log 2>/dev/null \
    && ok "auditoria em disco contém a ação, sem dados sensíveis" || ko "auditoria em disco ausente/incorreta"
fechar_a

echo "== 5. cancelamento: durante aguardando aprovação =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"c1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.escrita","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"plano"' || ko "cancelamento: sem plano antes do cancelar"
EXEC_ANTES=$(wc -l < "$HOME_T/a.out")
enviar_a '{"protocolo":"lia.v1","tipo":"cancelar","id":"c2","em_resposta_a":null,"ts":"t","dados":{"execucao_id":""}}'
aguardar "$HOME_T/a.out" '"estado":"cancelado"' && ok "cancelar durante aguardando_aprovacao -> resultado cancelado" \
    || ko "cancelamento durante aguardando_aprovacao não funcionou"
fechar_a

echo "== 6. cancelamento: durante execução simulada =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"c3","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.leitura","parametros":{}}}'
sleep 0.05
enviar_a '{"protocolo":"lia.v1","tipo":"cancelar","id":"c4","em_resposta_a":null,"ts":"t","dados":{"execucao_id":""}}'
aguardar "$HOME_T/a.out" '"estado":"cancelado"' && ok "cancelar durante execucao_simulacao -> resultado cancelado (não sucesso)" \
    || ko "cancelamento durante execução não preveniu o sucesso"
fechar_a

echo "== 7. JSON inválido desconecta com erro estruturado =="
abrir_conexao a
enviar_a 'isto nao e json valido {{{'
aguardar "$HOME_T/a.out" '"codigo":"mensagem_invalida"' && ok "JSON inválido -> erro com código estável" || ko "sem erro estruturado para JSON inválido"
sleep 0.3
kill -0 "$NC_PID_a" 2>/dev/null && ko "conexão não foi encerrada após JSON inválido" || ok "conexão encerrada após JSON inválido (framing comprometido)"
fechar_a

echo "== 8. mensagem fora de ordem (tipo de resposta do daemon, vindo do cliente) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"resultado","id":"x1","em_resposta_a":null,"ts":"t","dados":{"execucao_id":"e1","estado":"sucesso","codigo_saida":0,"dados":{}}}'
aguardar "$HOME_T/a.out" '"codigo":"estado_invalido"' && ok "tipo de mensagem do daemon vindo do cliente -> estado_invalido" \
    || ko "mensagem fora de ordem não foi rejeitada"
fechar_a

echo "== 9. 'aprovar' sem plano pendente -> estado_invalido =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"aprovar","id":"y1","em_resposta_a":null,"ts":"t","dados":{"plano_id":"nao-existe","decisao":"aprovar"}}'
aguardar "$HOME_T/a.out" '"codigo":"estado_invalido"' && ok "'aprovar' sem plano pendente -> estado_invalido" \
    || ko "'aprovar' sem plano pendente não foi rejeitado"
fechar_a

echo "== 10. id duplicado na mesma conexão =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"assinar","id":"dup-1","em_resposta_a":null,"ts":"t","dados":{"canais":["auditoria"]}}'
sleep 0.1
enviar_a '{"protocolo":"lia.v1","tipo":"assinar","id":"dup-1","em_resposta_a":null,"ts":"t","dados":{"canais":["auditoria"]}}'
aguardar "$HOME_T/a.out" '"codigo":"id_duplicado"' && ok "id repetido na mesma sessão -> id_duplicado" || ko "id duplicado não detectado"
fechar_a

echo "== 11. linha acima do limite de tamanho =="
abrir_conexao a
python3 -c "print('{\"protocolo\":\"lia.v1\",\"tipo\":\"assinar\",\"id\":\"l1\",\"ts\":\"t\",\"dados\":{\"canais\":[],\"x\":\"' + 'A'*70000 + '\"}}')" >&3 2>/dev/null \
    || perl -e 'print "{\"protocolo\":\"lia.v1\",\"tipo\":\"assinar\",\"id\":\"l1\",\"ts\":\"t\",\"dados\":{\"canais\":[],\"x\":\"" . ("A" x 70000) . "\"}}\n"' >&3
# A resposta de erro chega antes do daemon fechar a conexão, mas o `nc`
# relay via FIFO às vezes não termina de drenar o socket antes de ser
# encerrado (corrida inerente ao cliente de teste, não ao daemon) — por
# isso o sinal determinístico aqui é o log do próprio daemon, não o que o
# relay conseguiu escrever em a.out (que fica como evidência best-effort).
sleep 0.3
if grep -q '"codigo":"limite_excedido"' "$HOME_T/a.out"; then
    ok "linha acima do limite -> limite_excedido (visto na resposta)"
elif grep -q "linha longa\|linha acima do limite" "$HOME_T/sessiond.log"; then
    ok "linha acima do limite -> rejeitada e desconectada (confirmado no log do daemon)"
else
    ko "linha longa não foi rejeitada"
fi
sleep 0.2
kill -0 "$NC_PID_a" 2>/dev/null && ko "conexão não encerrada após linha acima do limite" || ok "conexão encerrada após linha acima do limite"
fechar_a

echo "== 12. cliente desconectado abruptamente: daemon continua servindo outros =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"assinar","id":"z1","em_resposta_a":null,"ts":"t","dados":{"canais":["auditoria"]}}'
sleep 0.2
kill -9 "$NC_PID_a" 2>/dev/null
sleep 0.3
abrir_conexao b
enviar_b '{"protocolo":"lia.v1","tipo":"solicitar","id":"z2","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.leitura","parametros":{}}}'
aguardar "$HOME_T/b.out" '"tipo":"resultado"' && ok "daemon continua funcional após desconexão abrupta de outro cliente" \
    || ko "daemon não respondeu a um novo cliente após desconexão abrupta"
fechar_b

echo "== 13. dois clientes simultâneos, sem cruzamento de estado =="
abrir_conexao a
abrir_conexao b
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"s-a","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.leitura","parametros":{}}}'
enviar_b '{"protocolo":"lia.v1","tipo":"solicitar","id":"s-b","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.escrita","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"resultado"' || ko "cliente A (simultâneo) não completou"
aguardar "$HOME_T/b.out" '"tipo":"plano"' || ko "cliente B (simultâneo) não recebeu plano"
grep -q "mock.escrita" "$HOME_T/a.out" && ko "cliente A viu dados do pedido de B (cruzamento de estado)" \
    || ok "dois clientes simultâneos não cruzam estado (A nunca vê 'mock.escrita' de B)"
grep -q '"ferramenta":"mock.leitura"' "$HOME_T/b.out" && ko "cliente B viu dados do pedido de A" \
    || ok "cliente B não vê o pedido de A"
fechar_a
fechar_b

echo "== 14. cliente com UID inválido (quando o ambiente permitir) =="
if sudo -n -u nobody true >/dev/null 2>&1; then
    OUT_UID="$HOME_T/uid_invalido.out"
    sudo -n -u nobody sh -c "printf '{\"protocolo\":\"lia.v1\",\"tipo\":\"assinar\",\"id\":\"u1\",\"ts\":\"t\",\"dados\":{\"canais\":[]}}\n' | timeout 2 nc -U '$SOCK'" > "$OUT_UID" 2>&1
    # Duas defesas independentes podem bloquear isto, e qualquer uma basta:
    # o modo 0600 do arquivo de socket já impede 'nobody' de conectar no
    # nível do sistema de arquivos (nc nem chega a trocar bytes); se
    # chegasse a conectar, o SO_PEERCRED do daemon enviaria 'nao_autorizado'
    # antes de processar qualquer mensagem. Uma resposta "normal" (plano/
    # resultado) seria a falha real aqui.
    if grep -q '"codigo":"nao_autorizado"' "$OUT_UID"; then
        ok "conexão de uid diferente -> nao_autorizado (rejeitado pelo SO_PEERCRED do daemon)"
    elif [ ! -s "$OUT_UID" ] || grep -qiE 'permission denied|connection refused' "$OUT_UID"; then
        ok "conexão de uid diferente -> recusada antes de qualquer dado (permissão 0600 do socket)"
    else
        ko "conexão de uid diferente obteve resposta inesperada: $(head -c 200 "$OUT_UID")"
    fi
else
    echo "(sudo -n -u nobody indisponível neste ambiente — pulando o teste de UID inválido)"
fi

echo "== 15. socket stale é detectado e removido =="
parar_daemon
rm -f "$SOCK"
nc -lU "$SOCK" >/dev/null 2>&1 &
NC_STALE_PID=$!
sleep 0.3
kill -9 "$NC_STALE_PID" 2>/dev/null
wait "$NC_STALE_PID" 2>/dev/null
[ -S "$SOCK" ] && ok "socket 'stale' (sem listener) está presente antes de reiniciar" || ko "não foi possível preparar o socket stale para o teste"
iniciar_daemon
if [ -S "$SOCK" ]; then
    aguardar "$HOME_T/sessiond.log" "removido socket stale" && ok "daemon detectou e removeu o socket stale" \
        || ko "log não confirma remoção do socket stale"
    abrir_conexao a
    enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"st1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.leitura","parametros":{}}}'
    aguardar "$HOME_T/a.out" '"tipo":"resultado"' && ok "socket novo funciona depois de remover o stale" || ko "socket novo não funciona após stale"
    fechar_a
else
    ko "daemon não conseguiu escutar após encontrar socket stale"
fi

echo "== 16. ausência do daemon: GUI continua funcional =="
parar_daemon
rm -f "$SOCK"
if [ -x "$GUI" ]; then
    env HOME="$HOME_T" "$GUI" > "$HOME_T/gui_sem_daemon.log" 2>&1 &
    GUI_PID=$!
    sleep 1.5
    if kill -0 "$GUI_PID" 2>/dev/null; then
        grep -q "sem daemon" "$HOME_T/gui_sem_daemon.log" && ok "GUI sem daemon: detecta e mostra 'sem daemon', continua viva" \
            || ko "GUI sem daemon: não relatou claramente a ausência do daemon"
        kill "$GUI_PID" 2>/dev/null; wait "$GUI_PID" 2>/dev/null
    else
        ko "GUI encerrou sozinha na ausência do daemon (deveria continuar, só desconectada)"
    fi
else
    echo "(build/lia-gui não encontrado — rode 'make gui' para incluir este teste)"
fi

echo "== 17. daemon encerrado (SIGTERM) durante uma sessão: não trava =="
iniciar_daemon
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"w1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.escrita","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"plano"' || ko "sessão não chegou a 'plano' antes do encerramento"
kill -TERM "$DAEMON_PID"
i=0
while kill -0 "$DAEMON_PID" 2>/dev/null && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i+1)); done
kill -0 "$DAEMON_PID" 2>/dev/null && ko "daemon não encerrou a tempo após SIGTERM durante uma sessão" \
    || ok "daemon encerra corretamente com SIGTERM mesmo com uma sessão em andamento"
[ -S "$SOCK" ] && ko "socket não foi removido após o encerramento" || ok "socket removido após SIGTERM"
fechar_a
DAEMON_PID=""

printf '\n%d testes, %d falhas\n' "$total" "$falhas"
[ "$falhas" -eq 0 ]
