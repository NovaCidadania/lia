#!/bin/sh
set -eu

RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
HOME_T=$(mktemp -d /tmp/lia-web-teste.XXXXXX)
cleanup() { kill "${PID:-}" 2>/dev/null || true; wait "${PID:-}" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

env HOME="$HOME_T" LIA_SESSIOND_MODELO=mock "$RAIZ/build/lia-sessiond" >"$HOME_T/sessiond.log" 2>&1 &
PID=$!
i=0
while [ ! -S "$HOME_T/.lia/run/session.sock" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$HOME_T/.lia/run/session.sock" ] || { cat "$HOME_T/sessiond.log"; exit 1; }

SOCKET="$HOME_T/.lia/run/session.sock" python3 - <<'PY'
import json, os, socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(30)
s.connect(os.environ["SOCKET"])
buf = b""
def send(tipo, ident, dados):
    s.sendall((json.dumps({"protocolo":"lia.v1", "tipo":tipo, "id":ident,
                           "em_resposta_a":None, "ts":"t", "dados":dados}) + "\n").encode())
def recv():
    global buf
    while b"\n" not in buf: buf += s.recv(65536)
    line, buf = buf.split(b"\n", 1)
    return json.loads(line)
send("perguntar", "p1", {"mensagem":"pesquise na internet a cotação do dolar em relação ao real"})
proposta = recv()
assert proposta["tipo"] == "proposta"
assert "plano_proposto" in proposta["dados"], proposta
plano = proposta["dados"]["plano_proposto"]
assert plano is not None, proposta
assert plano["passos"][0]["ferramenta"] == "web.pesquisar"
assert plano["risco"] == "baixo" and plano["requer_aprovacao"] is True
send("solicitar", "s1", {"plano_id":plano["plano_id"], "hash":plano["hash"]})
assert recv()["tipo"] == "plano"
send("aprovar", "a1", {"plano_id":plano["plano_id"], "hash":plano["hash"],
                         "decisao":"aprovar", "escopo":"uma_vez"})
resultado = None
for _ in range(12):
    msg = recv()
    if msg["tipo"] == "resultado":
        resultado = msg
        if msg["dados"].get("estado") in ("sucesso", "falha", "cancelado"): break
assert resultado is not None and resultado["dados"]["estado"] == "sucesso"
dados = resultado["dados"]["dados"]["tarefa"]["passos"][0].get("dados")
assert dados and (isinstance(dados.get("cotacoes"), list) or isinstance(dados.get("resultados"), list))
if dados.get("cotacoes"):
    assert dados["cotacoes"][0]["valor"] > 0 and dados["cotacoes"][0]["data"]
    print("web: cotação HTTPS real, valor e data validados")
else:
    assert dados["fonte"] == "DuckDuckGo HTML"
    assert dados["resultados"] and all(x["url"].startswith("https://") for x in dados["resultados"])
    print("web: consulta HTTPS real, fontes e URLs validados")
PY
