#!/usr/bin/env python3
"""Pesquisa web de ponta a ponta, SEM rede: daemon com executor real e
LIA_WEB_FIXTURES_PARA_TESTE (páginas e busca vindas de arquivos — ver
web/pesquisa.c, web_get_fixture), modelo mock ou servidor Ollama falso.

Confere o fluxo inteiro: pergunta -> consulta limpa -> plano -> aprovação
presa ao hash -> busca + abertura das fontes -> resposta final CONCLUSIVA e
ESTRUTURADA (Resposta / Pontos principais / Fontes com URL e data),
conferida contra as fontes; e os caminhos de falha (rede, HTTP 500, busca
vazia, redirecionamento recusado, síntese inválida, cancelamento).

Sempre em HOME temporário sob /tmp. Uso: make teste-web-resposta"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SESSIOND = os.path.join(RAIZ, "build", "lia-sessiond")
FAKE = os.path.join(RAIZ, "testes", "integration", "ollama_fake.py")
total = falhas = 0


def checar(cond, desc, detalhe=""):
    global total, falhas
    total += 1
    if cond:
        print("ok   " + desc, flush=True)
    else:
        falhas += 1
        print("FALHA " + desc + (("\n      " + str(detalhe)[:1500]) if detalhe != "" else ""), flush=True)


def nome_fixture(url):
    """mesma regra de web/pesquisa.c (fixture_nome)"""
    sem = re.sub(r"^https://", "", url).split("?")[0].split("#")[0]
    n = re.sub(r"[^A-Za-z0-9]", "_", sem)
    return n.rstrip("_")


def busca_html(resultados):
    partes = []
    for titulo, url, resumo in resultados:
        href = "//duckduckgo.com/l/?uddg=" + url.replace(":", "%3A").replace("/", "%2F") + "&amp;rut=x"
        partes.append('<div class="result"><a class="result__a" href="%s">%s</a>'
                      '<a class="result__snippet" href="%s">%s</a></div>' % (href, titulo, href, resumo))
    return "<html><body>%s</body></html>" % "".join(partes)


PAGINA_ENCICLOPEDIA = """<html><head><title>Exemplar (linguagem) - Enciclopédia</title>
<meta property="article:published_time" content="2024-05-01T10:00:00Z"></head><body>
<nav>Início | Menu | Contato</nav>
<p>A linguagem Exemplar é funcional e foi criada em 1990 por um comitê acadêmico de várias universidades.</p>
<p>Em 1998 saiu o padrão Exemplar 98, que se tornou a base do ensino da linguagem e de muitas pesquisas.</p>
<p>Receitas de bolo de cenoura com cobertura de chocolate são populares em festas de família no Brasil inteiro.</p>
<footer>Todos os direitos reservados</footer></body></html>"""

PAGINA_BLOG = """<html><head><title>História da Exemplar</title></head><body>
<p>O compilador principal da linguagem Exemplar ganhou suporte a concorrência em 2010, segundo a equipe do projeto.</p>
<p>Hoje a linguagem Exemplar é usada em pesquisa acadêmica e por algumas empresas de tecnologia financeira.</p>
</body></html>"""


def montar_fixtures(dir_, busca=None, paginas=None, extras=None):
    os.makedirs(dir_, exist_ok=True)
    if busca is not None:
        open(os.path.join(dir_, "busca"), "w").write(busca)
    for url, html in (paginas or {}).items():
        open(os.path.join(dir_, nome_fixture(url)), "w").write(html)
    for nome, conteudo in (extras or {}).items():
        open(os.path.join(dir_, nome), "w").write(conteudo)


RESULTADOS_PADRAO = [
    ("Exemplar (linguagem) - Enciclopédia", "https://enciclopedia.exemplo/exemplar", "A linguagem Exemplar é funcional..."),
    ("História da Exemplar", "https://blog.exemplo/historia", "O compilador ganhou concorrência em 2010."),
    ("PDF sobre Exemplar", "https://pdf.exemplo/exemplar.pdf", "Um PDF que não abre."),
]


def iniciar(home, fixtures, **env_extra):
    env = {"HOME": home, "PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LIA_SESSIOND_EXECUTOR": "real",
           "LIA_WEB_FIXTURES_PARA_TESTE": fixtures}
    for k in ("LIA_SESSIOND_SEM_RLIMIT_PARA_TESTE", "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"):
        if os.environ.get(k):
            env[k] = os.environ[k]
    env.update(env_extra)
    p = subprocess.Popen([SESSIOND], env=env, stdout=open(os.path.join(home, "sessiond.log"), "a"), stderr=subprocess.STDOUT,
                         start_new_session=True)
    sock = os.path.join(home, ".lia", "run", "session.sock")
    for _ in range(200):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    return p


def parar(p):
    if p and p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p.kill()


class Conexao:
    def __init__(self, home):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(os.path.join(home, ".lia", "run", "session.sock"))
        self.buf = b""
        self.n = 0

    def enviar(self, tipo, dados):
        self.n += 1
        self.s.sendall((json.dumps({"protocolo": "lia.v1", "tipo": tipo, "id": "w%d" % self.n, "em_resposta_a": None,
                                    "ts": "t", "dados": dados}) + "\n").encode())

    def ate(self, tipo, limite=20.0):
        todas = []
        fim = time.time() + limite
        while time.time() < fim:
            while b"\n" not in self.buf:
                resto = fim - time.time()
                if resto <= 0:
                    return todas, None
                self.s.settimeout(resto)
                try:
                    d = self.s.recv(65536)
                except socket.timeout:
                    return todas, None
                if not d:
                    return todas, None
                self.buf += d
            linha, self.buf = self.buf.split(b"\n", 1)
            m = json.loads(linha)
            todas.append(m)
            if m["tipo"] == tipo:
                return todas, m
        return todas, None

    def silencio(self, seg):
        msgs, m = self.ate("__nunca__", seg)
        return msgs

    def fechar(self):
        self.s.close()


def perguntar_e_executar(c, pergunta, contexto=None, limite=30):
    """pergunta -> proposta -> solicitar -> plano -> aprovar(hash) -> resultado -> resposta"""
    d = {"mensagem": pergunta}
    if contexto:
        d["contexto"] = contexto
    c.enviar("perguntar", d)
    _, pr = c.ate("proposta")
    pl = pr["dados"].get("plano_proposto") if pr else None
    if not pl:
        return pr, None, None, None, []
    c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": pr["dados"]["pergunta_id"]})
    _, plano = c.ate("plano")
    if plano and plano["dados"].get("requer_aprovacao"):
        c.enviar("aprovar", {"plano_id": plano["dados"]["plano_id"], "hash": plano["dados"]["hash"], "decisao": "aprovar", "escopo": "uma_vez"})
    msgs1, res = c.ate("resultado", limite)
    msgs2, resp = c.ate("resposta", limite)
    return pr, pl, res, resp, msgs1 + msgs2


def main():
    if not os.access(SESSIOND, os.X_OK):
        print("lia-sessiond não encontrado — rode 'make teste-web-resposta'")
        return 1
    base = tempfile.mkdtemp(prefix="lia-webr-", dir="/tmp")
    assert base.startswith("/tmp/") and base != os.path.expanduser("~")
    print("ambiente isolado:", base)
    daemons = []
    fake = None
    try:
        # ------------------------------------------------ cenário padrão
        fx = os.path.join(base, "fx1")
        montar_fixtures(fx, busca_html(RESULTADOS_PADRAO),
                        {"https://enciclopedia.exemplo/exemplar": PAGINA_ENCICLOPEDIA, "https://blog.exemplo/historia": PAGINA_BLOG},
                        {nome_fixture("https://pdf.exemplo/exemplar.pdf") + ".status": "404"})
        h1 = os.path.join(base, "h1"); os.makedirs(h1)
        d1 = iniciar(h1, fx); daemons.append(d1)
        c = Conexao(h1)

        print("== W1. pergunta sem '?' ('me conte com detalhes…') vai para a web, com a consulta limpa ==")
        pr, pl, res, resp, msgs = perguntar_e_executar(c, "Me conte com detalhes a evolução da linguagem Exemplar")
        checar(pl and pl["passos"][0]["ferramenta"] == "web.pesquisar", "W1: pedido de conhecimento sem interrogação vira pesquisa web", pr)
        par = pl["passos"][0]["parametros"] if pl else {}
        checar(par.get("consulta") == "evolução da linguagem Exemplar" and par.get("abrir_fontes") == 3,
               "W1: consulta sem o 'me conte com detalhes a' e com abertura de 3 fontes", par)
        checar(res and res["dados"]["estado"] == "sucesso", "W1: coleta concluída", res)

        print("== W2. resposta final conclusiva, estruturada e conferida ==")
        t = resp["dados"]["resposta_texto"] if resp else ""
        checar(resp and resp["dados"]["origem"] == "modelo_sintese", "W2: síntese do modelo aceita", resp and resp["dados"])
        checar(t.startswith("Resposta: (mock) síntese conclusiva: A linguagem Exemplar é funcional e foi criada em 1990"),
               "W2: começa pela resposta conclusiva", t[:300])
        checar("Pontos principais:" in t and "[1]" in t and "[2]" in t, "W2: pontos principais citando as fontes", t)
        checar("Fontes:\n  [1] Exemplar (linguagem) - Enciclopédia\n      https://enciclopedia.exemplo/exemplar" in t,
               "W2: fontes numeradas com título e URL", t)
        checar("data declarada pela página: 2024-05-01" in t and "a página não declara data" in t, "W2: data da página (ou a ausência dela)")
        checar("bolo de cenoura" not in t and "Todos os direitos" not in t, "W2: conteúdo irrelevante da página não entra na resposta")
        checar("pdf.exemplo" not in t, "W2: fonte que não abriu (HTTP 404) não aparece como base da resposta")
        checar("Como esta resposta foi montada:" in t, "W2: diz como a resposta foi montada")
        r = res["dados"]["dados"]["tarefa"]["passos"][0]["dados"] if res else {}
        abertas = [x for x in r.get("resultados", []) if x.get("aberta")]
        pdf = [x for x in r.get("resultados", []) if "pdf.exemplo" in x["url"]]
        checar(r.get("fontes_abertas") == 2 and len(abertas) == 2 and all(x.get("conteudo") for x in abertas),
               "W2: contrato de dados — resultados lidos trazem 'conteudo'", r)
        checar(pdf and pdf[0]["aberta"] is False and "HTTP 404" in pdf[0].get("erro_abertura", ""), "W2: falha ao abrir registrada em 'erro_abertura'", pdf)

        print("== W3. continuação usa o assunto da pergunta anterior (nunca o texto da Lia) ==")
        ctx = "Usuário: Me conte com detalhes a evolução da linguagem Exemplar\nLia: Resposta: (texto longo da Lia que não pode ir para a busca)\n"
        c.enviar("perguntar", {"mensagem": "e quando ela foi criada?", "contexto": ctx})
        _, pr = c.ate("proposta")
        q = pr["dados"]["plano_proposto"]["passos"][0]["parametros"]["consulta"] if pr and pr["dados"].get("plano_proposto") else ""
        checar(q == "evolução da linguagem Exemplar e quando ela foi criada", "W3: consulta de continuação = assunto anterior + pergunta", q)
        checar("Lia" not in q and "\n" not in q, "W3: nada do texto da Lia nem quebras de linha na consulta")
        c.enviar("perguntar", {"mensagem": "Me fale sobre o estado atual da linguagem Exemplar", "contexto": ctx})
        _, pr = c.ate("proposta")
        q = pr["dados"]["plano_proposto"]["passos"][0]["parametros"]["consulta"] if pr and pr["dados"].get("plano_proposto") else ""
        checar(q == "estado atual da linguagem Exemplar", "W3: pergunta autocontida não herda contexto", q)

        print("== W4. duas pesquisas seguidas na mesma conexão ==")
        _, _, res2, resp2, _ = perguntar_e_executar(c, "Fale sobre a história da linguagem Exemplar")
        checar(res2 and resp2 and resp2["dados"]["resposta_texto"].startswith("Resposta:"), "W4: segunda pesquisa também termina com resposta conclusiva")

        print("== W5. síntese com números inventados: descartados, nunca exibidos ==")
        _, _, _, resp, _ = perguntar_e_executar(c, "Me conte sobre a linguagem Exemplar MOCK_SINTESE_INVENTADA")
        t = resp["dados"]["resposta_texto"] if resp else ""
        checar(t and "1777" not in t and "9.99" not in t and "2031" not in t and "bolo" not in t, "W5: nada inventado chega à resposta", t)
        checar(resp and resp["dados"]["origem"] == "extrativa" and "frases copiadas dos trechos" in t and t.startswith("Resposta:"),
               "W5: sem nada aproveitável na síntese, a resposta extrativa (ainda conclusiva) assume", resp and resp["dados"])

        print("== W6. síntese que não é JSON: resposta extrativa, com o motivo ==")
        _, _, _, resp, _ = perguntar_e_executar(c, "Me conte sobre a linguagem Exemplar MOCK_SINTESE_INVALIDA")
        t = resp["dados"]["resposta_texto"] if resp else ""
        checar(resp and resp["dados"]["origem"] == "extrativa" and t.startswith("Resposta: A linguagem Exemplar é funcional") and "[1]" in t,
               "W6: resposta extrativa conclusiva e citada", t[:300])
        checar(resp and "não devolveu o JSON" in (resp["dados"].get("aviso") or ""), "W6: o aviso diz por que o modelo não foi usado", resp and resp["dados"])

        print("== W7. pergunta local seguida de pergunta web ==")
        c.enviar("perguntar", {"mensagem": "Quais interfaces de rede existem?"})
        _, pr = c.ate("proposta")
        checar(pr and not pr["dados"].get("plano_proposto") and pr["dados"]["ferramenta_proposta"]["ferramenta"] == "sistema.dispositivos",
               "W7: pergunta sobre esta máquina continua local (sistema.dispositivos), não vai para a web")
        _, _, _, resp, _ = perguntar_e_executar(c, "Explique a linguagem Exemplar")
        checar(resp and resp["dados"]["resposta_texto"].startswith("Resposta:"), "W7: em seguida, a pergunta web funciona normalmente")
        c.fechar()
        parar(d1)

        print("== W8. cotação: resposta determinística com valor, par, data, fonte e URL ==")
        fx = os.path.join(base, "fx2")
        montar_fixtures(fx, extras={"cambio": json.dumps({"amount": 1.0, "base": "USD", "date": "2026-11-13", "rates": {"BRL": 5.6637}})})
        h = os.path.join(base, "h2"); os.makedirs(h)
        d = iniciar(h, fx); daemons.append(d)
        c = Conexao(h)
        _, _, res, resp, _ = perguntar_e_executar(c, "Qual o preço atual do dólar em reais?")
        t = resp["dados"]["resposta_texto"] if resp else ""
        checar(t.startswith("Resposta: 1 USD = 5,6637 BRL (dólar americano para real brasileiro)") and "data de referência 2026-11-13" in t and
               "https://api.frankfurter.dev/v1/latest?base=USD&symbols=BRL" in t, "W8: cotação conclusiva com data, fonte e URL", t)
        checar(resp and resp["dados"]["origem"] == "deterministica", "W8: cotação não passa pelo modelo")
        c.fechar()
        parar(d)

        print("== W9. falha de rede e HTTP 500: erro real, nada inventado ==")
        for nome, extra, esperado in (("timeout", {"busca.erro": "tempo esgotado (timeout)"}, "tempo esgotado"),
                                      ("http500", {"busca": "erro", "busca.status": "500"}, "HTTP 500")):
            fx = os.path.join(base, "fx-" + nome)
            montar_fixtures(fx, extras=extra)
            h = os.path.join(base, "h-" + nome); os.makedirs(h)
            d = iniciar(h, fx); daemons.append(d)
            c = Conexao(h)
            _, _, res, resp, _ = perguntar_e_executar(c, "Me conte sobre a linguagem Exemplar")
            t = resp["dados"]["resposta_texto"] if resp else ""
            checar(res and res["dados"]["estado"] in ("incompleto", "falha"), "W9 (%s): a tarefa não é dada como sucesso" % nome, res and res["dados"]["estado"])
            checar(esperado in t and "Nada foi inventado" in t and "Pontos principais" not in t, "W9 (%s): a resposta mostra o erro real" % nome, t)
            c.fechar()
            parar(d)

        print("== W10. busca sem resultados ==")
        fx = os.path.join(base, "fx-vazio")
        montar_fixtures(fx, busca_html([]))
        h = os.path.join(base, "h-vazio"); os.makedirs(h)
        d = iniciar(h, fx); daemons.append(d)
        c = Conexao(h)
        _, _, res, resp, _ = perguntar_e_executar(c, "Me conte sobre a linguagem Inexistentíssima")
        t = resp["dados"]["resposta_texto"] if resp else ""
        checar("não encontrou fontes" in t and "Nada foi inventado" in t, "W10: diz que não encontrou fontes, sem inventar", t)
        c.fechar()
        parar(d)

        print("== W11. redirecionamentos: seguidos quando HTTPS público; recusados para http:// ==")
        fx = os.path.join(base, "fx-redir")
        montar_fixtures(fx, busca_html([("Velha", "https://velho.exemplo/pagina", "x"), ("Insegura", "https://seguro.exemplo/vai", "y")]),
                        {"https://novo.exemplo/pagina": PAGINA_ENCICLOPEDIA},
                        {nome_fixture("https://velho.exemplo/pagina") + ".status": "301",
                         nome_fixture("https://velho.exemplo/pagina") + ".location": "https://novo.exemplo/pagina",
                         nome_fixture("https://seguro.exemplo/vai") + ".status": "302",
                         nome_fixture("https://seguro.exemplo/vai") + ".location": "http://inseguro.exemplo/"})
        h = os.path.join(base, "h-redir"); os.makedirs(h)
        d = iniciar(h, fx); daemons.append(d)
        c = Conexao(h)
        _, _, res, resp, _ = perguntar_e_executar(c, "Me conte sobre a linguagem Exemplar")
        r = res["dados"]["dados"]["tarefa"]["passos"][0]["dados"] if res else {}
        por_url = {x["url"]: x for x in r.get("resultados", [])}
        v, s = por_url.get("https://velho.exemplo/pagina", {}), por_url.get("https://seguro.exemplo/vai", {})
        checar(v.get("aberta") is True and "criada em 1990" in v.get("conteudo", ""), "W11: redirecionamento 301 para HTTPS público é seguido", v)
        checar(s.get("aberta") is False and "redirecionamento recusado" in s.get("erro_abertura", ""), "W11: redirecionamento para http:// é recusado", s)
        c.fechar()
        parar(d)

        print("== W12. web.abrir com URL indicada: resposta sobre a página ==")
        fx = os.path.join(base, "fx-abrir")
        montar_fixtures(fx, paginas={"https://docs.exemplo/manual": PAGINA_BLOG})
        h = os.path.join(base, "h-abrir"); os.makedirs(h)
        d = iniciar(h, fx); daemons.append(d)
        c = Conexao(h)
        pr, pl, res, resp, _ = perguntar_e_executar(c, "Resuma o que diz https://docs.exemplo/manual sobre a linguagem Exemplar")
        t = resp["dados"]["resposta_texto"] if resp else ""
        checar(pl and pl["passos"][0]["ferramenta"] == "web.abrir", "W12: URL indicada usa web.abrir", pl)
        checar(t.startswith("Resposta:") and "https://docs.exemplo/manual" in t and "História da Exemplar" in t, "W12: resposta cita a página aberta", t)
        c.fechar()
        parar(d)

        print("== W13. modelo real (Ollama falso): progresso da escrita, cancelamento e resposta extrativa quando o texto não é JSON ==")
        fx = os.path.join(base, "fx1")
        porta_arq = os.path.join(base, "fake.port")
        fake = subprocess.Popen([sys.executable, FAKE, "0"], env=dict(os.environ, OLLAMA_FAKE_MODO="lento"),
                                stdout=open(porta_arq, "w"), stderr=subprocess.DEVNULL, start_new_session=True)
        for _ in range(100):
            if os.path.getsize(porta_arq) > 0:
                break
            time.sleep(0.05)
        porta = open(porta_arq).read().split()[0]
        h = os.path.join(base, "h-ollama"); os.makedirs(h)
        d = iniciar(h, fx, LIA_SESSIOND_MODELO="ollama", LIA_OLLAMA_PORTA=porta, LIA_OLLAMA_MODELO="falso"); daemons.append(d)
        c = Conexao(h)
        pr, pl, res, resp, msgs = perguntar_e_executar(c, "Me conte sobre a linguagem Exemplar", limite=40)
        prog = [m for m in msgs if m["tipo"] == "progresso" and "sintese" in m["dados"]]
        checar(len(prog) >= 2 and prog[-1]["dados"]["sintese"]["tokens"] > prog[0]["dados"]["sintese"]["tokens"],
               "W13: andamento da escrita chega ao vivo (tokens crescentes, ≥2 eventos)", [p["dados"]["mensagem"] for p in prog])
        checar(all("execucao_id" not in p["dados"] for p in prog), "W13: progresso da síntese sem execucao_id (a coleta já terminou)")
        checar(resp and resp["dados"]["origem"] == "extrativa" and resp["dados"]["resposta_texto"].startswith("Resposta:"),
               "W13: o texto do modelo não era o JSON pedido -> resposta extrativa conclusiva", resp and resp["dados"])
        # cancelamento durante a síntese
        c.enviar("perguntar", {"mensagem": "Me conte sobre a linguagem Exemplar"})
        _, pr = c.ate("proposta")
        pl = pr["dados"]["plano_proposto"]
        c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": pr["dados"]["pergunta_id"]})
        _, plano = c.ate("plano")
        c.enviar("aprovar", {"plano_id": plano["dados"]["plano_id"], "hash": plano["dados"]["hash"], "decisao": "aprovar", "escopo": "uma_vez"})
        c.ate("resultado")
        c.ate("progresso", 10)
        c.enviar("cancelar", {"pergunta": True})
        _, err = c.ate("erro", 5)
        checar(err and err["dados"]["codigo"] == "cancelado", "W13: cancelar durante a síntese é confirmado", err)
        depois = c.silencio(4)
        checar(not any(m["tipo"] == "resposta" for m in depois), "W13: nenhuma resposta chega depois do cancelamento", [m["tipo"] for m in depois])
        c.fechar()
        parar(d)
    finally:
        for d in daemons:
            parar(d)
        if fake and fake.poll() is None:
            fake.terminate()
            fake.wait(timeout=5)

    restos = [p for p in subprocess.run(["pgrep", "-f", base], capture_output=True, text=True).stdout.split() if p != str(os.getpid())]
    checar(not restos, "nenhum processo residual", restos)
    if not os.environ.get("LIA_DEBUG_MANTER"):
        shutil.rmtree(base, ignore_errors=True)
    print("\n%d testes, %d falhas" % (total, falhas))
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
