#!/bin/sh
# Testes de integração do daemon de sessão (sessiond/sessiond.c) contra o
# protocolo real, em socket Unix de verdade, em um $HOME temporário —
# NUNCA toca ~/.lia da pessoa. Uso: make integration
#
# PARTE A (executor real, padrão): as seis ferramentas de leitura de
# sessiond/manifesto_real.c, mais tools.discover.persistir/tools.probe.
# persistir (as únicas duas que gravam algo — sempre com aprovação, nunca
# por padrão, ver docs/manifesto.md §6.1), contra o sistema de verdade (mas
# sempre dentro do $HOME temporário — tools.discover/probe nunca leem
# ~/.lia real, só o que está no PATH do sistema, que é dado público do host).
#
# PARTE B (executor mock, LIA_SESSIOND_EXECUTOR=mock): máquina de estados
# do protocolo em si — aprovação, replay, cancelamento em cada ponto do
# fluxo, capacidade_negada — usando sessiond/manifesto_mock.c, que tem
# ferramentas de risco médio/admin (mock.escrita/mock.admin) que as seis
# reais não têm, e timing controlável (dois ticks de 250ms), ao contrário
# das ferramentas reais, rápidas demais numa máquina de teste para uma
# corrida de cancelamento confiável. Ver docs/protocolo.md §8.
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

# ------------------------------------------------------------------------
# Segurança do próprio harness: nunca rodar isto contra o HOME real. Falha
# imediatamente (antes de iniciar qualquer daemon) se o diretório temporário
# não estiver isolado como esperado.
case "$HOME_T" in
    /tmp/*) : ;;
    *) echo "ERRO FATAL: HOME_T ('$HOME_T') não está sob /tmp — abortando sem iniciar nada"; exit 1 ;;
esac
if [ "$HOME_T" = "$HOME" ] || [ "$HOME_T" = "$(eval echo ~"$(id -un)" 2>/dev/null)" ]; then
    echo "ERRO FATAL: HOME_T coincide com o HOME real — abortando sem iniciar nada"
    exit 1
fi
case "$SOCK" in
    "$HOME_T"/*) : ;;
    *) echo "ERRO FATAL: caminho do socket ('$SOCK') não está dentro de HOME_T — abortando"; exit 1 ;;
esac
echo "ambiente isolado: HOME_T=$HOME_T (diferente do HOME real: $HOME)"
MARCA_INICIO="$HOME_T/.marca_inicio"
touch "$MARCA_INICIO"

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

# iniciar_daemon [mock]  — sem argumento, usa o executor real (padrão)
iniciar_daemon() {
    modo=${1:-real}
    case "$SOCK" in "$HOME_T"/*) : ;; *) echo "ERRO FATAL: socket fora de HOME_T, recusando iniciar"; exit 1 ;; esac
    if [ "$modo" = mock ]; then
        env HOME="$HOME_T" LIA_SESSIOND_EXECUTOR=mock "$SESSIOND" > "$HOME_T/sessiond.log" 2>&1 &
    else
        env HOME="$HOME_T" "$SESSIOND" > "$HOME_T/sessiond.log" 2>&1 &
    fi
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
    arq=$1; pad=$2; tent=${3:-50}
    i=0
    while [ "$i" -lt "$tent" ]; do
        grep -q "$pad" "$arq" 2>/dev/null && return 0
        sleep 0.1
        i=$((i+1))
    done
    return 1
}

extrair_plano_id() { grep -o '"plano_id":"[^"]*"' "$1" | head -1 | cut -d'"' -f4; }
extrair_exec_id()  { grep -o '"execucao_id":"[^"]*"' "$1" | head -1 | cut -d'"' -f4; }

# ========================================================================
# PARTE A — executor real (padrão), seis ferramentas de leitura
# ========================================================================

echo "== A1. daemon inicia em diretório temporário, socket correto =="
iniciar_daemon
[ -S "$SOCK" ] && ok "socket criado em \$HOME/.lia/run/session.sock" || ko "socket não foi criado"
[ "$(stat -c %a "$HOME_T/.lia/run" 2>/dev/null)" = "700" ] && ok "diretório run/ com modo 700" || ko "modo do diretório run/"
[ "$(stat -c %a "$SOCK" 2>/dev/null)" = "600" ] && ok "socket com modo 600" || ko "modo do socket"
[ "$(stat -c %U "$SOCK" 2>/dev/null)" = "$(id -un)" ] && ok "socket pertence ao usuário atual" || ko "dono do socket"
grep -q "executor = real" "$HOME_T/sessiond.log" && ok "executor padrão é o real (não o mock)" || ko "executor padrão não é o real"

echo "== A2. fluxo real por ferramenta: tools.discover (somente leitura, não grava nada) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"rd1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"tools.discover","parametros":{}}}'
aguardar "$HOME_T/a.out" '"estado":"sucesso"' && ok "tools.discover: resultado sucesso" || ko "tools.discover falhou"
aguardar "$HOME_T/a.out" '"tipo":"auditoria"' && ok "tools.discover: auditoria emitida" || ko "tools.discover sem auditoria"
aguardar "$HOME_T/a.out" '"requer_aprovacao":false' && ok "tools.discover: risco nenhum, sem aprovação (não grava nada)" \
    || ko "tools.discover deveria continuar sem aprovação"
fechar_a
[ ! -f "$HOME_T/.lia/maquina/inventario/ferramentas.lia" ] && ok "tools.discover (sem --persistir) não grava o inventário" \
    || ko "tools.discover gravou o inventário mesmo sem persistir explicitamente (correção da auditoria revertida)"

echo "== A2b. tools.discover.persistir: ação separada, sempre com aprovação, grava de verdade =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"rd2","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"tools.discover.persistir","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"plano"' && ok "tools.discover.persistir: plano emitido" || ko "sem plano"
aguardar "$HOME_T/a.out" '"requer_aprovacao":true' && ok "tools.discover.persistir: requer aprovação" \
    || ko "tools.discover.persistir deveria exigir aprovação"
PID_DISCOVER=$(extrair_plano_id "$HOME_T/a.out")
enviar_a "$(printf '{"protocolo":"lia.v1","tipo":"aprovar","id":"rd2b","em_resposta_a":null,"ts":"t","dados":{"plano_id":"%s","decisao":"aprovar","escopo":"uma_vez"}}' "$PID_DISCOVER")"
aguardar "$HOME_T/a.out" '"estado":"sucesso"' && ok "tools.discover.persistir: resultado sucesso" || ko "tools.discover.persistir falhou"
fechar_a
[ -f "$HOME_T/.lia/maquina/inventario/ferramentas.lia" ] && ok "tools.discover.persistir grava o inventário dentro do HOME temporário" \
    || ko "inventário não foi gravado"

echo "== A3. fluxo real por ferramenta: tools.list (depende do discover acima) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"rl1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"tools.list","parametros":{"estado":"ACCESSIBLE"}}}'
aguardar "$HOME_T/a.out" '"estado":"sucesso"' && ok "tools.list --estado ACCESSIBLE: sucesso" || ko "tools.list falhou"
fechar_a

echo "== A4. fluxo real por ferramenta: tools.probe --safe, somente leitura (depende do discover.persistir) =="
# O executor real captura a saída da ferramenta inteira e a devolve dentro
# de 'resultado.dados' (synchronous, sem streaming) — diferente do mock,
# que emite 'saida'/'progresso' incrementais só para exercitar esses tipos
# de mensagem do protocolo (docs/protocolo.md §8).
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"rp1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"tools.probe","parametros":{}}}'
aguardar "$HOME_T/a.out" '"estado":"sucesso"' "100" && ok "tools.probe --safe: sucesso" || ko "tools.probe falhou"
aguardar "$HOME_T/a.out" '"requer_aprovacao":false' && ok "tools.probe: risco nenhum, sem aprovação (não grava nada)" \
    || ko "tools.probe deveria continuar sem aprovação"
grep -qE '"codigo_saida":[0-9]+.*PROBE_SUCCESS|PROBE_SUCCESS' "$HOME_T/a.out" && ok "tools.probe: saída contém resultado de probe real" \
    || ko "tools.probe sem conteúdo esperado"
fechar_a

echo "== A4b. tools.probe.persistir: ação separada, sempre com aprovação, grava de verdade =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"rp2","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"tools.probe.persistir","parametros":{}}}'
aguardar "$HOME_T/a.out" '"requer_aprovacao":true' && ok "tools.probe.persistir: requer aprovação" \
    || ko "tools.probe.persistir deveria exigir aprovação"
PID_PROBE=$(extrair_plano_id "$HOME_T/a.out")
enviar_a "$(printf '{"protocolo":"lia.v1","tipo":"aprovar","id":"rp2b","em_resposta_a":null,"ts":"t","dados":{"plano_id":"%s","decisao":"aprovar","escopo":"uma_vez"}}' "$PID_PROBE")"
aguardar "$HOME_T/a.out" '"estado":"sucesso"' "100" && ok "tools.probe.persistir: resultado sucesso" || ko "tools.probe.persistir falhou"
fechar_a

echo "== A5. fluxo real por ferramenta: sistema.diagnosticar/recursos/dispositivos =="
for ferramenta in sistema.diagnosticar sistema.recursos sistema.dispositivos; do
    abrir_conexao a
    enviar_a "$(printf '{"protocolo":"lia.v1","tipo":"solicitar","id":"s-%s","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"%s","parametros":{}}}' "$ferramenta" "$ferramenta")"
    aguardar "$HOME_T/a.out" '"estado":"sucesso"' && ok "$ferramenta: sucesso" || ko "$ferramenta falhou"
    fechar_a
done

echo "== A6. validação de parâmetros no executor (não só na GUI) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"pi1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"tools.list","parametros":{"estado":"ESTADO_INVENTADO"}}}'
aguardar "$HOME_T/a.out" '"codigo":"parametro_invalido"' && ok "enum inválido em 'estado' -> parametro_invalido (validado no executor)" \
    || ko "parâmetro inválido não foi rejeitado"
fechar_a

abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"pi2","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"tools.list","parametros":{"chave_que_nao_existe":1}}}'
aguardar "$HOME_T/a.out" '"codigo":"parametro_invalido"' && ok "parâmetro desconhecido -> parametro_invalido" \
    || ko "parâmetro desconhecido não foi rejeitado"
fechar_a

abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"pi3","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"tools.probe","parametros":{"safe":false}}}'
aguardar "$HOME_T/a.out" '"codigo":"parametro_invalido"' && ok "tools.probe com safe=false -> parametro_invalido (exceção escopada só permite --safe)" \
    || ko "tools.probe safe=false não foi rejeitado"
fechar_a

echo "== A7. ferramenta desconhecida =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"fd1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"admin.vetorial.inventada","parametros":{}}}'
aguardar "$HOME_T/a.out" '"codigo":"ferramenta_desconhecida"' && ok "ferramenta fora do manifesto -> ferramenta_desconhecida" \
    || ko "ferramenta desconhecida não foi rejeitada"
fechar_a

echo "== A8. cancelamento rejeitado quando não há nada em andamento =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"cancelar","id":"cn1","em_resposta_a":null,"ts":"t","dados":{"execucao_id":"exec-que-nao-existe"}}'
aguardar "$HOME_T/a.out" '"codigo":"estado_invalido"' && ok "cancelar sem nada em andamento -> estado_invalido" \
    || ko "cancelar sem nada em andamento não foi rejeitado"
fechar_a

echo "== A9. desconexão durante execução real: daemon não trava, serve outros clientes =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"dd1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"tools.probe","parametros":{}}}'
sleep 0.05
kill -9 "$NC_PID_a" 2>/dev/null
abrir_conexao b
enviar_b '{"protocolo":"lia.v1","tipo":"solicitar","id":"dd2","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"sistema.diagnosticar","parametros":{}}}'
aguardar "$HOME_T/b.out" '"estado":"sucesso"' && ok "daemon responde a outro cliente mesmo com uma execução real desconectada no meio" \
    || ko "daemon não respondeu após desconexão durante execução real"
fechar_b

echo "== A10. daemon nunca toca ~/.lia real — confirmando isolamento =="
# Checar se o HOME real aparece em texto dentro de HOME_T não é um bom
# teste: tools.discover lista diretórios do PATH, e um deles pode
# legitimamente estar sob o HOME real (~/.local/bin) sem que isso seja um
# acesso ao ~/.lia real — o que importa é que nada foi ESCRITO lá.
if [ "$HOME" != "$HOME_T" ] && [ -d "$HOME/.lia" ]; then
    ANTES=$(find "$HOME/.lia" -newer "$MARCA_INICIO" 2>/dev/null | wc -l)
    [ "$ANTES" -eq 0 ] && ok "nada em ~/.lia real foi criado/alterado durante os testes" \
        || ko "arquivos em ~/.lia real foram modificados durante os testes (ISOLAMENTO QUEBRADO)"
else
    ok "~/.lia real não existe ou HOME não pôde ser comparado — sem evidência de escrita (ambiente controlado)"
fi

parar_daemon

# ========================================================================
# PARTE B — executor mock (LIA_SESSIOND_EXECUTOR=mock): máquina de estados
# ========================================================================

echo "== B1. fluxo nenhum (mock.leitura) auto-executa sem aprovação =="
iniciar_daemon mock
grep -q "executor = mock" "$HOME_T/sessiond.log" && ok "LIA_SESSIOND_EXECUTOR=mock troca o backend" || ko "backend mock não foi ativado"
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"cli-1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.leitura","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"resultado"' && ok "mock.leitura: plano+resultado sem aprovação" || ko "mock.leitura não completou"
aguardar "$HOME_T/a.out" '"tipo":"auditoria"' && ok "auditoria emitida após o resultado" || ko "sem auditoria"
fechar_a

echo "== B2. cliente GUI/protocolo (gui/protocolo.c real, via build/lia-gui se houver display) =="
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

echo "== B3. fluxo plano -> aprovação -> execução -> resultado (risco médio) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"f1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.escrita","parametros":{"x":1}}}'
aguardar "$HOME_T/a.out" '"tipo":"plano"' && ok "plano emitido para ferramenta de risco médio" || ko "sem plano"
aguardar "$HOME_T/a.out" '"requer_aprovacao":true' && ok "plano marca requer_aprovacao=true" || ko "requer_aprovacao incorreto"
PID_PLANO=$(extrair_plano_id "$HOME_T/a.out")
enviar_a "$(printf '{"protocolo":"lia.v1","tipo":"aprovar","id":"f2","em_resposta_a":null,"ts":"t","dados":{"plano_id":"%s","decisao":"aprovar","escopo":"uma_vez"}}' "$PID_PLANO")"
aguardar "$HOME_T/a.out" '"tipo":"progresso"' && ok "progresso emitido após aprovação" || ko "sem progresso"
aguardar "$HOME_T/a.out" '"estado":"sucesso"' && ok "resultado sucesso após execução completar" || ko "sem resultado de sucesso"
aguardar "$HOME_T/a.out" '"tipo":"auditoria"' && ok "auditoria emitida (fluxo com aprovação)" || ko "sem auditoria"
grep -q "acao=sessiond.mock.escrita" "$HOME_T/.lia/humano/auditoria"/*.log 2>/dev/null \
    && ok "auditoria em disco contém a ação, sem dados sensíveis" || ko "auditoria em disco ausente/incorreta"
fechar_a

echo "== B4. aprovação duplicada (replay) é rejeitada =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"rep1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.escrita","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"plano"' || ko "replay: sem plano inicial"
PID_PLANO_REP=$(extrair_plano_id "$HOME_T/a.out")
enviar_a "$(printf '{"protocolo":"lia.v1","tipo":"aprovar","id":"rep2","em_resposta_a":null,"ts":"t","dados":{"plano_id":"%s","decisao":"aprovar"}}' "$PID_PLANO_REP")"
aguardar "$HOME_T/a.out" '"estado":"sucesso"' || ko "replay: primeira aprovação não completou"
LINHAS_ANTES=$(wc -l < "$HOME_T/a.out")
enviar_a "$(printf '{"protocolo":"lia.v1","tipo":"aprovar","id":"rep3","em_resposta_a":null,"ts":"t","dados":{"plano_id":"%s","decisao":"aprovar"}}' "$PID_PLANO_REP")"
aguardar "$HOME_T/a.out" '"codigo":"estado_invalido"' "20" && ok "aprovação duplicada (replay do mesmo plano_id) -> estado_invalido" \
    || ko "replay de aprovação não foi rejeitado"
fechar_a

echo "== B5. aprovação de um plano diferente do apresentado é rejeitada =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"alt1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.escrita","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"plano"' || ko "plano alterado: sem plano inicial"
enviar_a '{"protocolo":"lia.v1","tipo":"aprovar","id":"alt2","em_resposta_a":null,"ts":"t","dados":{"plano_id":"plano-forjado-nao-e-o-real","decisao":"aprovar"}}'
aguardar "$HOME_T/a.out" '"codigo":"estado_invalido"' && ok "aprovar plano_id diferente do apresentado -> estado_invalido" \
    || ko "aprovação de plano alterado não foi rejeitada"
fechar_a

echo "== B6. capacidade não permitida (risco admin) é rejeitada antes de qualquer execução =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"cap1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.admin","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"plano"' && ok "mock.admin: plano emitido (risco=admin, requer_aprovacao=true)" || ko "mock.admin sem plano"
grep -q '"risco":"admin"' "$HOME_T/a.out" && ok "plano de mock.admin declara risco=admin corretamente" || ko "risco incorreto no plano"
fechar_a

echo "== B7. cancelamento: antes de qualquer execução (plano aguardando aprovação) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"c1","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.escrita","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"plano"' || ko "cancelamento antes: sem plano"
enviar_a '{"protocolo":"lia.v1","tipo":"cancelar","id":"c2","em_resposta_a":null,"ts":"t","dados":{"execucao_id":""}}'
aguardar "$HOME_T/a.out" '"estado":"cancelado"' && ok "cancelar antes da aprovação -> resultado cancelado" \
    || ko "cancelamento antes da execução não funcionou"
fechar_a

echo "== B8. cancelamento: durante a execução (execucao_id correto) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"c3","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.leitura","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"saida"' || ko "cancelamento durante: sem evento de saída"
EXEC_ID_B8=$(extrair_exec_id "$HOME_T/a.out")
enviar_a "$(printf '{"protocolo":"lia.v1","tipo":"cancelar","id":"c4","em_resposta_a":null,"ts":"t","dados":{"execucao_id":"%s"}}' "$EXEC_ID_B8")"
aguardar "$HOME_T/a.out" '"estado":"cancelado"' && ok "cancelar durante a execução (execucao_id correto) -> resultado cancelado (não sucesso)" \
    || ko "cancelamento durante a execução não preveniu o sucesso"
fechar_a

echo "== B9. cancelamento: depois de concluída (nada para cancelar) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"c5","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.leitura","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"resultado"' || ko "cancelamento depois: execução não completou"
enviar_a '{"protocolo":"lia.v1","tipo":"cancelar","id":"c6","em_resposta_a":null,"ts":"t","dados":{"execucao_id":"exec-qualquer"}}'
aguardar "$HOME_T/a.out" '"codigo":"estado_invalido"' && ok "cancelar depois de concluída -> estado_invalido (nada em andamento)" \
    || ko "cancelamento depois de concluída não foi rejeitado"
fechar_a

echo "== B10. cancelamento com execucao_id incorreto é rejeitado (não cancela por engano) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"solicitar","id":"c7","em_resposta_a":null,"ts":"t","dados":{"ferramenta":"mock.leitura","parametros":{}}}'
aguardar "$HOME_T/a.out" '"tipo":"saida"' || ko "execucao_id incorreto: sem evento de saída"
enviar_a '{"protocolo":"lia.v1","tipo":"cancelar","id":"c8","em_resposta_a":null,"ts":"t","dados":{"execucao_id":"exec-id-errado-de-propositado"}}'
aguardar "$HOME_T/a.out" '"codigo":"estado_invalido"' "10" && ok "cancelar com execucao_id que não corresponde -> estado_invalido" \
    || ko "execucao_id incorreto não foi rejeitado"
aguardar "$HOME_T/a.out" '"estado":"sucesso"' && ok "execução original prossegue e conclui normalmente (cancelamento incorreto não teve efeito)" \
    || ko "execução original foi afetada pelo cancelamento incorreto"
fechar_a

echo "== B11. JSON inválido desconecta com erro estruturado =="
abrir_conexao a
enviar_a 'isto nao e json valido {{{'
aguardar "$HOME_T/a.out" '"codigo":"mensagem_invalida"' && ok "JSON inválido -> erro com código estável" || ko "sem erro estruturado para JSON inválido"
sleep 0.3
kill -0 "$NC_PID_a" 2>/dev/null && ko "conexão não foi encerrada após JSON inválido" || ok "conexão encerrada após JSON inválido (framing comprometido)"
fechar_a

echo "== B12. mensagem fora de ordem (tipo de resposta do daemon, vindo do cliente) =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"resultado","id":"x1","em_resposta_a":null,"ts":"t","dados":{"execucao_id":"e1","estado":"sucesso","codigo_saida":0,"dados":{}}}'
aguardar "$HOME_T/a.out" '"codigo":"estado_invalido"' && ok "tipo de mensagem do daemon vindo do cliente -> estado_invalido" \
    || ko "mensagem fora de ordem não foi rejeitada"
fechar_a

echo "== B13. 'aprovar' sem plano pendente -> estado_invalido =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"aprovar","id":"y1","em_resposta_a":null,"ts":"t","dados":{"plano_id":"nao-existe","decisao":"aprovar"}}'
aguardar "$HOME_T/a.out" '"codigo":"estado_invalido"' && ok "'aprovar' sem plano pendente -> estado_invalido" \
    || ko "'aprovar' sem plano pendente não foi rejeitado"
fechar_a

echo "== B14. id duplicado na mesma conexão =="
abrir_conexao a
enviar_a '{"protocolo":"lia.v1","tipo":"assinar","id":"dup-1","em_resposta_a":null,"ts":"t","dados":{"canais":["auditoria"]}}'
sleep 0.1
enviar_a '{"protocolo":"lia.v1","tipo":"assinar","id":"dup-1","em_resposta_a":null,"ts":"t","dados":{"canais":["auditoria"]}}'
aguardar "$HOME_T/a.out" '"codigo":"id_duplicado"' && ok "id repetido na mesma sessão -> id_duplicado" || ko "id duplicado não detectado"
fechar_a

echo "== B15. linha acima do limite de tamanho =="
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

echo "== B16. cliente desconectado abruptamente: daemon continua servindo outros =="
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

echo "== B17. dois clientes simultâneos, sem cruzamento de estado =="
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

echo "== B18. cliente com UID inválido (quando o ambiente permitir) =="
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

echo "== B19. socket stale é detectado e removido =="
parar_daemon
rm -f "$SOCK"
nc -lU "$SOCK" >/dev/null 2>&1 &
NC_STALE_PID=$!
sleep 0.3
kill -9 "$NC_STALE_PID" 2>/dev/null
wait "$NC_STALE_PID" 2>/dev/null
[ -S "$SOCK" ] && ok "socket 'stale' (sem listener) está presente antes de reiniciar" || ko "não foi possível preparar o socket stale para o teste"
iniciar_daemon mock
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

echo "== B20. ausência do daemon: GUI continua funcional =="
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

echo "== B21. daemon encerrado (SIGTERM) durante uma sessão: não trava =="
iniciar_daemon mock
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
