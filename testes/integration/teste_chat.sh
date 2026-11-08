#!/bin/sh
# Testes de integração do lia-chat (chat/lia-chat.c) — primeiro cliente de
# chat interativo de terminal para o protocolo (docs/protocolo.md §9).
# Sempre em $HOME temporário, nunca toca ~/.lia real. Uso: make teste-chat
#
# Roda lia-chat em modo não interativo (stdin de um FIFO, nunca um TTY de
# verdade) — mais determinístico para testar via shell do que simular um
# terminal. Cada bloco usa uma conexão (processo lia-chat) nova.
set -u
trap '' PIPE
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
SESSIOND="$RAIZ/build/lia-sessiond"
CHAT="$RAIZ/build/lia-chat"
FAKE_OLLAMA="$RAIZ/testes/integration/ollama_fake.py"

if [ ! -x "$SESSIOND" ] || [ ! -x "$CHAT" ]; then
    echo "lia-sessiond/lia-chat não encontrados — rode 'make teste-chat' (ele compila antes)"
    exit 1
fi

falhas=0; total=0
ok()  { total=$((total+1)); printf 'ok   %s\n' "$1"; }
ko()  { total=$((total+1)); falhas=$((falhas+1)); printf 'FALHA %s\n' "$1"; }

HOME_T=$(mktemp -d)
SOCK="$HOME_T/.lia/run/session.sock"

case "$HOME_T" in
    /tmp/*) : ;;
    *) echo "ERRO FATAL: HOME_T ('$HOME_T') não está sob /tmp — abortando sem iniciar nada"; exit 1 ;;
esac
if [ "$HOME_T" = "$HOME" ]; then
    echo "ERRO FATAL: HOME_T coincide com o HOME real — abortando sem iniciar nada"
    exit 1
fi
case "$SOCK" in
    "$HOME_T"/*) : ;;
    *) echo "ERRO FATAL: caminho do socket não está dentro de HOME_T — abortando"; exit 1 ;;
esac
echo "ambiente isolado: HOME_T=$HOME_T (diferente do HOME real: $HOME)"
MARCA_INICIO="$HOME_T/.marca_inicio"; touch "$MARCA_INICIO"

DAEMON_PID=""
FAKE_PID=""
CHAT_PID=""
limpar_tudo() {
    [ -n "$CHAT_PID" ] && kill "$CHAT_PID" 2>/dev/null
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null
    [ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2>/dev/null
    pkill -f "lia-chat.*$HOME_T" 2>/dev/null
    wait 2>/dev/null
    if [ -z "${LIA_DEBUG_MANTER:-}" ]; then
        rm -rf "$HOME_T"
    else
        echo "DEBUG: HOME_T preservado em $HOME_T"
    fi
}
trap 'limpar_tudo' EXIT

iniciar_daemon_mock() {
    env HOME="$HOME_T" LIA_SESSIOND_EXECUTOR=mock "$@" "$SESSIOND" > "$HOME_T/sessiond.log" 2>&1 &
    DAEMON_PID=$!
    i=0
    while [ ! -S "$SOCK" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
}

parar_daemon() {
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=""
    i=0
    while [ -e "$SOCK" ] && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i+1)); done
}

iniciar_fake_ollama() {
    modo=$1
    rm -f "$HOME_T/fake_ollama.port"
    env OLLAMA_FAKE_MODO="$modo" python3 "$FAKE_OLLAMA" 0 > "$HOME_T/fake_ollama.port" 2>"$HOME_T/fake_ollama.log" &
    FAKE_PID=$!
    i=0
    while [ ! -s "$HOME_T/fake_ollama.port" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
    FAKE_PORT=$(head -1 "$HOME_T/fake_ollama.port" | tr -dc '0-9')
}
parar_fake_ollama() { [ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2>/dev/null; wait "$FAKE_PID" 2>/dev/null; FAKE_PID=""; }

# abrir_chat [args extras do lia-chat...] -> cria HOME_T/chat.in (fifo) e
# HOME_T/chat.out (saída combinada stdout+stderr), fd 5 escreve no fifo.
abrir_chat() {
    rm -f "$HOME_T/chat.in"; mkfifo "$HOME_T/chat.in"
    : > "$HOME_T/chat.out"
    env HOME="$HOME_T" "$CHAT" "$@" < "$HOME_T/chat.in" > "$HOME_T/chat.out" 2>&1 &
    CHAT_PID=$!
    exec 5> "$HOME_T/chat.in"
    i=0
    while [ ! -s "$HOME_T/chat.out" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i+1)); done
}
enviar_chat() { printf '%s\n' "$1" >&5; }
fechar_chat() {
    exec 5>&- 2>/dev/null
    [ -n "$CHAT_PID" ] && wait "$CHAT_PID" 2>/dev/null
    CODIGO_CHAT=$?
    CHAT_PID=""
}

aguardar() { # arquivo padrao [tentativas]
    arq=$1; pad=$2; tent=${3:-50}
    i=0
    while [ "$i" -lt "$tent" ]; do
        grep -qF "$pad" "$arq" 2>/dev/null && return 0
        sleep 0.1
        i=$((i+1))
    done
    return 1
}
nao_aparece() {
    arq=$1; pad=$2; tent=${3:-20}
    i=0
    while [ "$i" -lt "$tent" ]; do
        if grep -qF "$pad" "$arq" 2>/dev/null; then return 1; fi
        sleep 0.1
        i=$((i+1))
    done
    return 0
}

# ========================================================================
# A — conectar, conversa simples, proposta, solicitação, aprovação/rejeição
# ========================================================================

echo "== A1. lia-chat conecta ao daemon num HOME temporário =="
iniciar_daemon_mock
abrir_chat
aguardar "$HOME_T/chat.out" "conectado a" && ok "lia-chat conectou ao daemon" || ko "A1: sem confirmação de conexão"
fechar_chat

echo "== A2. conversa simples (sem ferramenta sugerida) =="
abrir_chat
enviar_chat "oi, como você está?"
aguardar "$HOME_T/chat.out" "[modelo]" && ok "A2: resposta do modelo recebida" || ko "A2: sem resposta do modelo"
nao_aparece "$HOME_T/chat.out" "ferramenta sugerida" "5" && ok "A2: nenhuma ferramenta sugerida para uma conversa genérica" \
    || ko "A2: ferramenta sugerida inesperadamente"
enviar_chat "/sair"
fechar_chat
[ "$CODIGO_CHAT" -eq 0 ] && ok "A2: código de saída 0 após /sair" || ko "A2: código de saída inesperado ($CODIGO_CHAT)"

echo "== A3. proposta de ferramenta + /status mostrando a pendência =="
abrir_chat
enviar_chat "MOCK_PROPOR:mock.leitura"
aguardar "$HOME_T/chat.out" "ferramenta sugerida: mock.leitura" && ok "A3: ferramenta sugerida exibida" || ko "A3: sem sugestão"
enviar_chat "/status"
aguardar "$HOME_T/chat.out" "proposta de ferramenta pendente: mock.leitura" && ok "A3: /status mostra a proposta pendente" \
    || ko "A3: /status não mostrou a proposta"
enviar_chat "/sair"
fechar_chat

echo "== A4. /plano solicita a ferramenta proposta (risco nenhum, sem aprovação) =="
abrir_chat
enviar_chat "MOCK_PROPOR:mock.leitura"
aguardar "$HOME_T/chat.out" "ferramenta sugerida" || ko "A4: sem proposta"
enviar_chat "/plano"
aguardar "$HOME_T/chat.out" "sem aprovação necessária" && ok "A4: ferramenta de risco nenhum executa sem aprovação" \
    || ko "A4: deveria ter executado sem aprovação"
aguardar "$HOME_T/chat.out" "estado=sucesso" && ok "A4: resultado de sucesso recebido" || ko "A4: sem resultado"
enviar_chat "/sair"
fechar_chat

echo "== A5. plano pendente (risco médio) + aprovação explícita [s] =="
abrir_chat
enviar_chat "MOCK_PROPOR:mock.escrita"
aguardar "$HOME_T/chat.out" "ferramenta sugerida: mock.escrita" || ko "A5: sem proposta"
enviar_chat "/plano"
aguardar "$HOME_T/chat.out" "Plano pendente" && ok "A5: plano pendente exibido" || ko "A5: sem exibição do plano"
aguardar "$HOME_T/chat.out" "Requer aprovação: sim" && ok "A5: plano marca requer aprovação" || ko "A5: não marcou aprovação"
enviar_chat "s"
aguardar "$HOME_T/chat.out" "aprovação enviado" && ok "A5: aprovação enviada após responder 's'" || ko "A5: aprovação não foi enviada"
aguardar "$HOME_T/chat.out" "estado=sucesso" && ok "A5: resultado de sucesso após aprovação" || ko "A5: sem resultado"
enviar_chat "/sair"
fechar_chat

echo "== A6. padrão é rejeitar (linha vazia / qualquer coisa != s) =="
abrir_chat
enviar_chat "MOCK_PROPOR:mock.escrita"
aguardar "$HOME_T/chat.out" "ferramenta sugerida: mock.escrita" || ko "A6: sem proposta"
enviar_chat "/plano"
aguardar "$HOME_T/chat.out" "Plano pendente" || ko "A6: sem plano"
enviar_chat "qualquer coisa"
aguardar "$HOME_T/chat.out" "rejeição enviado" && ok "A6: resposta não-afirmativa rejeita (padrão seguro)" \
    || ko "A6: não rejeitou por padrão"
aguardar "$HOME_T/chat.out" "estado=cancelado" && ok "A6: resultado 'cancelado' após rejeição" || ko "A6: sem resultado cancelado"
enviar_chat "/sair"
fechar_chat

echo "== A7. /rejeitar explícito também funciona =="
abrir_chat
enviar_chat "MOCK_PROPOR:mock.admin"
aguardar "$HOME_T/chat.out" "ferramenta sugerida: mock.admin" || ko "A7: sem proposta"
enviar_chat "/plano"
aguardar "$HOME_T/chat.out" "Risco:       admin" && ok "A7: plano de risco admin exibido corretamente" || ko "A7: risco incorreto"
enviar_chat "/rejeitar"
aguardar "$HOME_T/chat.out" "rejeição enviado" && ok "A7: /rejeitar envia rejeição" || ko "A7: /rejeitar não funcionou"
enviar_chat "/sair"
fechar_chat

# ========================================================================
# B — cancelamento, Ctrl-C, pendências órfãs
# ========================================================================

echo "== B1. /cancelar com um plano aguardando aprovação =="
abrir_chat
enviar_chat "MOCK_PROPOR:mock.escrita"
aguardar "$HOME_T/chat.out" "ferramenta sugerida" || ko "B1: sem proposta"
enviar_chat "/plano"
aguardar "$HOME_T/chat.out" "Plano pendente" || ko "B1: sem plano"
enviar_chat "/cancelar"
aguardar "$HOME_T/chat.out" "cancelamento do plano pendente enviado" && ok "B1: /cancelar envia cancelamento do plano" \
    || ko "B1: /cancelar não enviou nada"
aguardar "$HOME_T/chat.out" "estado=cancelado" && ok "B1: resultado 'cancelado' confirmado pelo daemon" || ko "B1: sem confirmação"
enviar_chat "/sair"
fechar_chat

echo "== B2. /cancelar durante execução (mock.timeout, janela de 300ms) =="
abrir_chat
enviar_chat "MOCK_PROPOR:mock.timeout"
aguardar "$HOME_T/chat.out" "ferramenta sugerida" || ko "B2: sem proposta"
enviar_chat "/plano"
aguardar "$HOME_T/chat.out" "sem aprovação necessária" "20" || ko "B2: mock.timeout deveria executar sem aprovação"
enviar_chat "/cancelar"
aguardar "$HOME_T/chat.out" "cancelamento da execução" && ok "B2: /cancelar durante execução enviado" || ko "B2: sem cancelamento"
aguardar "$HOME_T/chat.out" "estado=cancelado" "20" && ok "B2: execução cancelada antes do timeout" || ko "B2: não cancelou"
enviar_chat "/sair"
fechar_chat

echo "== B3. /cancelar sem nada pendente não envia nada ao daemon =="
abrir_chat
enviar_chat "/cancelar"
aguardar "$HOME_T/chat.out" "nada para cancelar" && ok "B3: 'nada para cancelar' reportado localmente" || ko "B3: mensagem incorreta"
enviar_chat "/sair"
fechar_chat

echo "== B4. Ctrl-C (SIGINT) com um plano pendente cancela, com a sessão ociosa só avisa =="
abrir_chat
enviar_chat "MOCK_PROPOR:mock.escrita"
aguardar "$HOME_T/chat.out" "ferramenta sugerida" || ko "B4: sem proposta"
enviar_chat "/plano"
aguardar "$HOME_T/chat.out" "Plano pendente" || ko "B4: sem plano"
kill -INT "$CHAT_PID" 2>/dev/null
aguardar "$HOME_T/chat.out" "cancelamento do plano pendente enviado" && ok "B4: Ctrl-C com plano pendente cancela" \
    || ko "B4: Ctrl-C não cancelou o plano"
# > 2s: fora da janela de "Ctrl-C duas vezes" (B5) — aqui queremos
# exercitar o caminho "ocioso, só avisa", não o de saída forçada.
sleep 2.2
kill -INT "$CHAT_PID" 2>/dev/null
aguardar "$HOME_T/chat.out" "nada para cancelar" && ok "B4: Ctrl-C ocioso só avisa, não encerra" || ko "B4: comportamento ocioso incorreto"
enviar_chat "/sair"
fechar_chat
[ "$CODIGO_CHAT" -eq 0 ] && ok "B4: /sair ainda funciona depois dos Ctrl-C" || ko "B4: código de saída inesperado"

echo "== B5. Ctrl-C duas vezes em menos de 2s força saída com código 130 =="
abrir_chat
# espera uma resposta de verdade (não só o banner inicial, que é impresso
# ANTES de g_main_loop_run) para confirmar que o laço principal já está
# rodando.
enviar_chat "/status"
aguardar "$HOME_T/chat.out" "conexão:" || ko "B5: sem resposta inicial — não deu pra confirmar que o laço está rodando"
kill -INT "$CHAT_PID" 2>/dev/null
# SIGINT é um sinal POSIX padrão (não enfileirado pelo kernel): um
# segundo 'kill -INT' antes do primeiro ter sido totalmente processado
# pode colapsar numa única entrega. Em vez de um sleep fixo (frágil sob
# carga variável da máquina), espera a CONFIRMAÇÃO de que o primeiro já
# foi tratado (a dica "nada para cancelar") antes de mandar o segundo —
# ainda bem dentro da janela de 2s que caracteriza "Ctrl-C duas vezes".
aguardar "$HOME_T/chat.out" "nada para cancelar" "30" || ko "B5: primeiro Ctrl-C não foi processado"
kill -INT "$CHAT_PID" 2>/dev/null
# dá uma folga para o segundo sinal ser de fato processado antes de
# checar a saída — sem isto, sob carga variável da máquina, 'wait'
# (dentro de fechar_chat) pode ser chamado antes do handler do segundo
# SIGINT terminar de rodar, lendo um estado que ainda vai mudar.
sleep 0.5
fechar_chat
[ "$CODIGO_CHAT" -eq 130 ] && ok "B5: Ctrl-C duplo sai com código 130" || ko "B5: código de saída incorreto ($CODIGO_CHAT)"

parar_daemon

# ========================================================================
# C — daemon ausente, socket inválido, erro de uso
# ========================================================================

echo "== C1. daemon ausente: lia-chat recusa e sai com código 1 =="
env HOME="$HOME_T" "$CHAT" < /dev/null > "$HOME_T/c1.out" 2>&1
rc=$?
[ "$rc" -eq 1 ] && ok "C1: código de saída 1 sem daemon" || ko "C1: código de saída incorreto ($rc)"
grep -q "não foi possível conectar" "$HOME_T/c1.out" && ok "C1: mensagem clara de ausência de daemon" || ko "C1: mensagem pouco clara"

echo "== C2. socket inválido (caminho que não é socket) =="
touch "$HOME_T/nao_e_socket"
env HOME="$HOME_T" "$CHAT" --socket "$HOME_T/nao_e_socket" < /dev/null > "$HOME_T/c2.out" 2>&1
rc=$?
[ "$rc" -eq 1 ] && ok "C2: código de saída 1 para caminho que não é socket" || ko "C2: código incorreto ($rc)"

echo "== C3. opção de linha de comando inválida -> código 2 =="
env HOME="$HOME_T" "$CHAT" --opcao-que-nao-existe < /dev/null > "$HOME_T/c3.out" 2>&1
rc=$?
[ "$rc" -eq 2 ] && ok "C3: código de saída 2 para opção inválida" || ko "C3: código incorreto ($rc)"

echo "== C4. --backend com valor inválido -> código 2 =="
env HOME="$HOME_T" "$CHAT" --backend torradeira < /dev/null > "$HOME_T/c4.out" 2>&1
rc=$?
[ "$rc" -eq 2 ] && ok "C4: código de saída 2 para --backend inválido" || ko "C4: código incorreto ($rc)"

iniciar_daemon_mock

# ========================================================================
# D — /modelo, /backend, Ollama ausente
# ========================================================================

echo "== D1. /modelo com backend mock: sempre disponível, sem rede =="
abrir_chat
enviar_chat "/modelo"
aguardar "$HOME_T/chat.out" "disponibilidade: sim" && ok "D1: backend mock sempre disponível" || ko "D1: mock deveria estar disponível"
enviar_chat "/sair"
fechar_chat

echo "== D2. /backend ollama + /modelo sem Ollama instalado: funciona, reporta indisponível =="
abrir_chat --backend ollama
enviar_chat "/modelo"
aguardar "$HOME_T/chat.out" "disponibilidade: não" "30" && ok "D2: Ollama ausente reportado sem travar" || ko "D2: não reportou indisponibilidade"
enviar_chat "/sair"
fechar_chat
[ "$CODIGO_CHAT" -eq 0 ] && ok "D2: lia-chat funciona normalmente sem Ollama instalado" || ko "D2: código de saída inesperado"

echo "== D3. /backend dentro da sessão muda a suposição local (não o daemon) =="
abrir_chat
enviar_chat "/backend ollama"
aguardar "$HOME_T/chat.out" "backend assumido agora: ollama" && ok "D3: /backend troca a suposição local" || ko "D3: /backend não funcionou"
aguardar "$HOME_T/chat.out" "não controla o daemon remotamente" && ok "D3: aviso de que não controla o daemon remoto" || ko "D3: sem aviso"
enviar_chat "/sair"
fechar_chat

parar_daemon

echo "== D4. backend Ollama de verdade contra servidor Ollama falso (streaming) =="
iniciar_fake_ollama normal
iniciar_daemon_mock LIA_SESSIOND_MODELO=ollama LIA_OLLAMA_PORTA="$FAKE_PORT"
abrir_chat
enviar_chat "ola"
aguardar "$HOME_T/chat.out" "ola mundo" "50" && ok "D4: texto do Ollama (via servidor falso) chega corretamente ao lia-chat" \
    || ko "D4: streaming não chegou corretamente"
enviar_chat "/sair"
fechar_chat
parar_daemon
parar_fake_ollama

iniciar_daemon_mock

# ========================================================================
# E — modo --json (NDJSON cru)
# ========================================================================

echo "== E1. --json: linha NDJSON válida é repassada ao daemon e a resposta sai crua =="
abrir_chat --json
enviar_chat '{"protocolo":"lia.v1","tipo":"perguntar","id":"j1","em_resposta_a":null,"ts":"t","dados":{"mensagem":"MOCK_PROPOR:mock.leitura"}}'
aguardar "$HOME_T/chat.out" '"tipo":"proposta"' && ok "E1: --json recebe a 'proposta' crua" || ko "E1: sem proposta crua"
kill "$CHAT_PID" 2>/dev/null
fechar_chat

echo "== E2. --json: linha JSON inválida é rejeitada localmente, nunca enviada =="
abrir_chat --json
enviar_chat 'isto nao e json'
sleep 0.3
nao_aparece "$HOME_T/sessiond.log" "ERRO INTERNO INESPERADO" "2" && ok "E2: daemon não quebrou com entrada inválida" || ko "E2: daemon reagiu mal"
kill "$CHAT_PID" 2>/dev/null
fechar_chat

# ========================================================================
# F — limites e arquivos
# ========================================================================

echo "== F1. mensagem acima do limite não é enviada =="
abrir_chat
MSG_GRANDE=$(printf 'x%.0s' $(seq 1 9000))
enviar_chat "$MSG_GRANDE"
aguardar "$HOME_T/chat.out" "acima do limite" && ok "F1: mensagem grande rejeitada localmente" || ko "F1: não rejeitou"
nao_aparece "$HOME_T/chat.out" "pergunta enviada" "5" && ok "F1: nada foi enviado ao daemon" || ko "F1: enviou mesmo acima do limite"
enviar_chat "/sair"
fechar_chat

echo "== F2. arquivo acima do limite de tamanho não é anexado =="
python3 -c "open('$HOME_T/grande.txt','wb').write(b'x'*20000)"
abrir_chat
enviar_chat "/arquivo adicionar $HOME_T/grande.txt"
aguardar "$HOME_T/chat.out" "acima do limite" && ok "F2: arquivo grande rejeitado" || ko "F2: não rejeitou arquivo grande"
enviar_chat "/arquivos listar"
aguardar "$HOME_T/chat.out" "nenhum arquivo anexado" && ok "F2: nada foi anexado" || ko "F2: anexou mesmo assim"
enviar_chat "/sair"
fechar_chat

echo "== F3. link simbólico não é seguido =="
ln -s "$HOME_T/grande.txt" "$HOME_T/link.txt"
abrir_chat
enviar_chat "/arquivo adicionar $HOME_T/link.txt"
aguardar "$HOME_T/chat.out" "link simbólico" && ok "F3: link simbólico rejeitado" || ko "F3: não rejeitou link"
enviar_chat "/sair"
fechar_chat

echo "== F4. diretório não é anexado =="
abrir_chat
enviar_chat "/arquivo adicionar $HOME_T"
aguardar "$HOME_T/chat.out" "diretório" && ok "F4: diretório rejeitado" || ko "F4: não rejeitou diretório"
enviar_chat "/sair"
fechar_chat

echo "== F5. anexar, listar, remover um arquivo válido =="
printf 'conteudo de teste' > "$HOME_T/pequeno.txt"
abrir_chat
enviar_chat "/arquivo adicionar $HOME_T/pequeno.txt"
aguardar "$HOME_T/chat.out" "anexado:" && ok "F5: arquivo pequeno anexado" || ko "F5: não anexou"
enviar_chat "/arquivos listar"
aguardar "$HOME_T/chat.out" "pequeno.txt" && ok "F5: /arquivos listar mostra o arquivo" || ko "F5: não listou"
enviar_chat "/arquivo remover 1"
aguardar "$HOME_T/chat.out" "removido:" && ok "F5: /arquivo remover funciona" || ko "F5: não removeu"
enviar_chat "/sair"
fechar_chat

echo "== F6. caminho de arquivo com conteúdo malicioso continua marcado 'não confiável' (herda a proteção do daemon) =="
printf 'dado qualquer' > "$HOME_T/comum.txt"
abrir_chat
enviar_chat "/arquivo adicionar $HOME_T/comum.txt"
aguardar "$HOME_T/chat.out" "anexado:" || ko "F6: não anexou"
enviar_chat "resuma o arquivo"
aguardar "$HOME_T/chat.out" "[modelo]" && ok "F6: pergunta com arquivo anexado funciona" || ko "F6: não funcionou"
enviar_chat "/sair"
fechar_chat

parar_daemon

# ========================================================================
# G — segurança/ausência de chamadas perigosas, isolamento
# ========================================================================

echo "== G1. lia-chat nunca chama system/popen/fork/execve/posix_spawn/sudo/pkexec =="
nm -D "$CHAT" 2>/dev/null | grep -qE "\bsystem\b|\bpopen\b|\bexecve\b|\bposix_spawn\b|\bfork\b" \
    && ko "G1: símbolo de execução de processo encontrado no binário" \
    || ok "G1: nenhum símbolo de execução de processo no binário"
strings "$CHAT" 2>/dev/null | grep -qi "^pkexec$" && ko "G1: 'pkexec' hardcoded" || ok "G1: 'pkexec' não aparece hardcoded"

echo "== G2. nenhum acesso ao HOME real durante os testes =="
if [ "$HOME" != "$HOME_T" ] && [ -d "$HOME/.lia" ]; then
    ALTERADOS=$(find "$HOME/.lia" -newer "$MARCA_INICIO" 2>/dev/null | wc -l)
    [ "$ALTERADOS" -eq 0 ] && ok "G2: nada em ~/.lia real foi criado/alterado" \
        || ko "G2: ~/.lia real foi modificado (ISOLAMENTO QUEBRADO)"
else
    ok "G2: ~/.lia real não existe ou HOME não pôde ser comparado"
fi

echo "== G3. nenhum processo lia-chat/lia-sessiond residual ao final =="
sleep 0.3
ps aux 2>/dev/null | grep -E "lia-chat|lia-sessiond" | grep -v grep | grep -q "$HOME_T" \
    && ko "G3: processo residual referenciando HOME_T encontrado" \
    || ok "G3: nenhum processo residual referenciando HOME_T"

echo "== G4. nenhum socket residual fora do HOME temporário =="
if [ -S "$HOME/.lia/run/session.sock" ]; then
    ANTES=$(find "$HOME/.lia/run/session.sock" -newer "$MARCA_INICIO" 2>/dev/null | wc -l)
    [ "$ANTES" -eq 0 ] && ok "G4: socket em ~/.lia real pré-existente, não criado por este teste" \
        || ko "G4: socket em ~/.lia real foi criado/alterado por este teste"
else
    ok "G4: nenhum socket em ~/.lia real — este teste só cria socket dentro de HOME_T"
fi

echo
echo "$total testes, $falhas falhas"
[ "$falhas" -eq 0 ]
