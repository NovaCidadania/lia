#!/usr/bin/env python3
"""Fase 2 de coding de ponta a ponta, SEM rede e SEM LLM: daemon real
(executor real), modelo mock roteirizado (LIA_MOCK_CODIGO), projeto C
temporário sob /tmp com um teste que começa falhando, validação isolada
(bubblewrap) e Git num repositório temporário.

Cobre: nada gravado antes da aprovação; aprovação → gravação → validação →
relatório com diff e prova; rejeição (edição e validação) com 'resposta'
final anunciada por resposta_a_seguir; arquivo alterado depois da prévia;
teste que continua falhando (novo ciclo); falha idêntica (repetição);
limite de ciclos; proposta inválida (caminho fora, arquivo não lido,
nenhuma edição); cancelamento; checkpoint e retomada; 'rode os testes';
git status/diff/log/commit (ganchos não rodam); lia-chat. Uso:
make teste-coding"""
import json, os, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from teste_web_resposta import Conexao, parar  # noqa: E402

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SESSIOND = os.path.join(RAIZ, "build", "lia-sessiond")
CHAT = os.path.join(RAIZ, "build", "lia-chat")
total = falhas = 0


def checar(cond, desc, detalhe=""):
    global total, falhas
    total += 1
    if cond:
        print("ok   " + desc, flush=True)
    else:
        falhas += 1
        print("FALHA " + desc + (("\n      " + str(detalhe)[:1800]) if detalhe != "" else ""), flush=True)


SOMA_BUG = "int soma(int a, int b) { return a - b; }\n"
TESTE = ('#include <stdio.h>\nint soma(int, int);\nint main(void) { if (soma(2, 3) != 5) { printf("FALHA soma(2,3)=%d\\n", soma(2, 3)); '
         'return 1; } puts("ok"); return 0; }\n')
MAKEFILE = "teste: teste_soma\n\t./teste_soma\nteste_soma: teste_soma.c soma.c\n\tcc -o $@ teste_soma.c soma.c\nlento:\n\tsleep 30\n"
PLANO = {"criterios": [{"descricao": "soma(2,3) devolve 5", "verificacao": "validacao"},
                       {"descricao": "soma.c usa a + b", "verificacao": "contem", "caminho": "soma.c", "texto": "a + b"}],
         "arquivos_para_ler": ["soma.c"], "validacao": "teste"}


def editar(de, para, caminho="soma.c"):
    return {"edicoes": [{"acao": "editar", "caminho": caminho, "procurar": de, "substituir": para}], "explicacao": "ajusta %s" % caminho}


def git(proj, *args):
    return subprocess.run(["git", "-C", proj, "-c", "user.name=Teste", "-c", "user.email=t@t"] + list(args),
                          capture_output=True, text=True).stdout


class Cenario:
    def __init__(self, base, nome, roteiro, extra_env=None):
        self.dir = os.path.join(base, nome)
        self.proj = os.path.join(self.dir, "proj")
        self.home = os.path.join(self.dir, "home")
        os.makedirs(self.proj)
        os.makedirs(self.home)
        for n, c in (("soma.c", SOMA_BUG), ("teste_soma.c", TESTE), ("Makefile", MAKEFILE), (".gitignore", "teste_soma\n")):
            with open(os.path.join(self.proj, n), "w") as f:
                f.write(c)
        git(self.proj, "init", "-q", "-b", "principal")
        git(self.proj, "add", ".")
        git(self.proj, "commit", "-q", "-m", "inicial")
        self.roteiro = os.path.join(self.dir, "roteiro.json")
        with open(self.roteiro, "w") as f:
            json.dump(roteiro, f)
        env = {"HOME": self.home, "PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LIA_SESSIOND_EXECUTOR": "real",
               "LIA_SESSIOND_PROJETO": self.proj, "LIA_MOCK_CODIGO": self.roteiro, "LIA_SESSIOND_GIT_AUTOR": "Lia Teste <lia@teste.local>"}
        for k in ("LIA_SESSIOND_SEM_RLIMIT_PARA_TESTE", "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"):
            if os.environ.get(k):
                env[k] = os.environ[k]
        env.update(extra_env or {})
        self.d = subprocess.Popen([SESSIOND], env=env, stdout=open(os.path.join(self.home, "sessiond.log"), "w"),
                                  stderr=subprocess.STDOUT, start_new_session=True)
        sock = os.path.join(self.home, ".lia", "run", "session.sock")
        for _ in range(200):
            if os.path.exists(sock):
                break
            time.sleep(0.05)
        self.c = Conexao(self.home)
        self.msgs = []

    def soma(self):
        return open(os.path.join(self.proj, "soma.c")).read()

    def ate(self, tipo, lim=30):
        msgs, m = self.c.ate(tipo, lim)
        self.msgs += msgs
        return m

    def perguntar(self, texto):
        self.c.enviar("perguntar", {"mensagem": texto})
        return self.ate("proposta")

    def decidir(self, plano, decisao):
        self.c.enviar("aprovar", {"plano_id": plano["dados"]["plano_id"], "hash": plano["dados"]["hash"], "decisao": decisao, "escopo": "uma_vez"})

    def progresso(self):
        return [m["dados"]["mensagem"] for m in self.msgs if m["tipo"] == "progresso" and "codigo" in m["dados"]]

    def checkpoint(self, cid):
        with open(os.path.join(self.home, ".lia", "projeto", "coding", cid + ".json")) as f:
            return json.load(f), os.stat(os.path.join(self.home, ".lia", "projeto", "coding", cid + ".json")).st_mode & 0o777

    def fim(self):
        self.c.fechar()
        parar(self.d)


def resposta_texto(r):
    return r["dados"]["resposta_texto"] if r else ""


def main():
    if not os.access(SESSIOND, os.X_OK):
        print("lia-sessiond não encontrado — rode 'make teste-coding'")
        return 1
    if subprocess.run(["/usr/bin/bwrap", "--unshare-all", "--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin", "/usr/bin/true"],
                      capture_output=True).returncode != 0:
        print("bubblewrap indisponível: a validação deve falhar fechada — conferido em teste-validacao-git")
    base = tempfile.mkdtemp(prefix="lia-coding-", dir="/tmp")
    poc_antes = subprocess.run(["git", "-C", "/home/zad/poc", "status", "--porcelain"], capture_output=True, text=True).stdout \
        if os.path.isdir("/home/zad/poc/.git") else None
    cenarios = []
    try:
        print("== C1. caminho completo: critérios → leitura → prévia → aprovação → validação → relatório ==")
        s = Cenario(base, "c1", {"plano": PLANO, "edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        pr = s.perguntar("corrija o bug na função soma em soma.c")
        checar(pr and pr["dados"].get("codigo") is True and pr["dados"].get("codigo_id", "").startswith("codigo-"), "C1: pedido de código reconhecido", pr)
        plano = s.ate("plano")
        prev = (plano["dados"].get("previas") or [{}])[0].get("previa", "") if plano else ""
        checar(plano and plano["dados"]["intencao"] == "codigo_edicao" and plano["dados"]["requer_aprovacao"] is True and
               "-int soma(int a, int b) { return a - b; }" in prev and "+int soma(int a, int b) { return a + b; }" in prev,
               "C1: proposta vira plano de gravação com prévia exata", plano and plano["dados"])
        checar(s.soma() == SOMA_BUG, "C1: NADA gravado antes da aprovação")
        passos = plano["dados"]["passos"] if plano else []
        checar(passos and passos[0]["ferramenta"] == "arquivo.escrever" and len(passos[0]["parametros"].get("sha256_esperado", "")) == 64,
               "C1: edição presa ao sha256 observado na leitura", passos)
        prog = s.progresso()
        checar(any("critérios de aceite" in p for p in prog) and any(p.startswith("li soma.c") for p in prog) and
               any("proposta pronta" in p for p in prog), "C1: progresso por etapa (critérios, leitura, proposta)", prog)
        etapas = [m["dados"]["codigo"]["etapa"] for m in s.msgs if m["tipo"] == "progresso" and "codigo" in m["dados"]]
        li = [i for i, p in enumerate(prog) if p.startswith("li ")]
        cr = [i for i, p in enumerate(prog) if "critérios de aceite" in p]
        checar(li and cr and max(li) < min(cr) and "contexto" in etapas and etapas.index("contexto") < etapas.index("criterios") < etapas.index("proposta"),
               "C1: o contexto real é lido ANTES dos critérios e da proposta (nenhum pedido ao modelo antes disso)", etapas)
        s.decidir(plano, "aprovar")
        pv = s.ate("plano")
        prevv = (pv["dados"].get("previas") or [{}])[0].get("previa", "") if pv else ""
        checar(pv and pv["dados"]["intencao"] == "codigo_validacao" and "executável: /usr/bin/make" in prevv and "argv: [\"make\",\"teste\"]" in prevv and
               "executa código do próprio projeto" in prevv and "sem rede" in prevv, "C1: validação pede aprovação mostrando comando, isolamento e o aviso", prevv)
        checar("a + b" in s.soma(), "C1: gravado depois da aprovação")
        s.decidir(pv, "aprovar")
        r = s.ate("resposta", 60)
        t = resposta_texto(r)
        checar(t.startswith("Resposta: concluído") and "✓ a validação \"teste\" passa depois da última edição" in t and "soma.c usa a + b" not in t,
               "C1: concluído com os critérios DO PEDIDO conferidos (os do modelo não entram sem o filtro)", t)
        checar("Contexto lido: soma.c, teste_soma.c" in t and "Origem das propostas:\n  ciclo 1: modelo local (mock)" in t,
               "C1: relatório diz o contexto lido (o arquivo citado e o teste dele) e a origem de cada proposta", t)
        checar("Diff:" in t and "+int soma(int a, int b) { return a + b; }" in t and "Validação: \"teste\" passou" in t and "depois da última edição" in t and "ok" in t,
               "C1: relatório cita o diff e a saída da validação posterior à edição", t)
        checar("desfaça a operação" in t and "Checkpoint: codigo-" in t, "C1: como desfazer e o checkpoint")
        cid = t.split("Checkpoint: ")[1].split()[0]
        ck, modo = s.checkpoint(cid)
        checar(ck["estado"] == "concluido" and ck["validacao_passou"] and modo == 0o600 and "return a" not in json.dumps(ck)
               and len(ck["operacoes"]) == 1, "C1: checkpoint privado (0600), sem conteúdo de arquivo, com a operação para desfazer", ck)
        s.fim()

        print("== C2. rejeitar a gravação: nada muda; 'resposta' final anunciada ==")
        s = Cenario(base, "c2", {"plano": PLANO, "edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c")
        plano = s.ate("plano")
        s.decidir(plano, "rejeitar")
        res = s.ate("resultado")
        r = s.ate("resposta")
        checar(res and res["dados"]["estado"] == "cancelado" and res["dados"].get("resposta_a_seguir") is True, "C2: resultado cancelado avisa resposta_a_seguir", res)
        checar(resposta_texto(r).startswith("Resposta: NÃO concluído — você recusou a gravação") and s.soma() == SOMA_BUG,
               "C2: resposta final explica; arquivo intacto", resposta_texto(r))
        s.fim()

        print("== C3. arquivo alterado depois da prévia: nada é sobrescrito ==")
        s = Cenario(base, "c3", {"plano": PLANO, "edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c")
        plano = s.ate("plano")
        with open(os.path.join(s.proj, "soma.c"), "a") as f:
            f.write("/* editado por outra pessoa */\n")
        mudado = s.soma()
        s.decidir(plano, "aprovar")
        r = s.ate("resposta")
        checar(s.soma() == mudado and "NÃO concluído" in resposta_texto(r) and "mudaram desde a prévia" in resposta_texto(r),
               "C3: edição de fora preservada, relatório explica", resposta_texto(r))
        s.fim()

        print("== C4. teste ainda falhando: novo ciclo com a saída do erro, depois passa ==")
        s = Cenario(base, "c4", {"plano": PLANO, "edicoes": [editar("a - b", "a * b"), editar("a * b", "a + b")]})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c")
        for i in range(2):
            p = s.ate("plano")
            s.decidir(p, "aprovar")
            v = s.ate("plano")
            s.decidir(v, "aprovar")
        r = s.ate("resposta", 60)
        t = resposta_texto(r)
        prog = s.progresso()
        checar(any("a validação falhou; pedindo uma correção" in p for p in prog) and any("ciclo 2 de 3" in p for p in prog),
               "C4: falha leva a um segundo ciclo, com nova prévia e aprovação", prog)
        checar(t.startswith("Resposta: concluído") and "2 gravação(ões)" in t and "a + b" in s.soma(), "C4: concluído só depois da validação passar", t)
        s.fim()

        print("== C5. falha idêntica depois da correção: repetição sem progresso ==")
        s = Cenario(base, "c5", {"plano": PLANO, "edicoes": [editar("a - b", "a * b"), editar("a * b", "b * a")]})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c")
        for i in range(2):
            p = s.ate("plano")
            s.decidir(p, "aprovar")
            v = s.ate("plano")
            s.decidir(v, "aprovar")
        r = s.ate("resposta", 60)
        checar("NÃO concluído — repetição sem progresso" in resposta_texto(r) and "✗ a validação \"teste\" passa" in resposta_texto(r),
               "C5: para e explica; nunca declara concluído", resposta_texto(r))
        s.fim()

        print("== C6. limite de ciclos ==")
        s = Cenario(base, "c6", {"plano": PLANO, "edicoes": [editar("a - b", "a * b"), editar("a * b", "a * b + 1"), editar("a * b + 1", "a * b + 2")]})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c")
        for i in range(3):
            p = s.ate("plano")
            s.decidir(p, "aprovar")
            v = s.ate("plano")
            s.decidir(v, "aprovar")
        r = s.ate("resposta", 60)
        checar("NÃO concluído — limite de 3 ciclos" in resposta_texto(r), "C6: limite de ciclos respeitado", resposta_texto(r))
        s.fim()

        print("== C7. propostas inválidas: fora do projeto, arquivo não lido, nenhuma edição ==")
        for nome, rot, trecho in (
            ("c7a", [{"edicoes": [{"acao": "criar", "caminho": "../fora.c", "conteudo": "x"}]}], "fora do projeto"),
            ("c7b", [editar("teste:", "teste2:", "Makefile")], "não foi lido nesta tarefa"),
            ("c7c", [{"edicoes": [], "explicacao": "nada"}], "nenhuma edição"),
        ):
            s = Cenario(base, nome, {"plano": PLANO, "edicoes": rot})
            cenarios.append(s)
            s.perguntar("corrija o bug na função soma em soma.c")
            r = s.ate("resposta")
            t = resposta_texto(r)
            checar("NÃO concluído — o modelo não produziu uma proposta válida em duas tentativas" in t and trecho in t and
                   "Diff: nenhum arquivo foi alterado" in t and s.soma() == SOMA_BUG and not os.path.exists(os.path.join(s.dir, "fora.c")),
                   "C7 (%s): recusada pelo verificador, nada gravado" % trecho, t)
            s.fim()

        print("== C8. recusar a validação, depois retomar pelo checkpoint ==")
        s = Cenario(base, "c8", {"plano": PLANO, "edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c")
        p = s.ate("plano")
        s.decidir(p, "aprovar")
        v = s.ate("plano")
        s.decidir(v, "rejeitar")
        r = s.ate("resposta")
        t = resposta_texto(r)
        checar("NÃO concluído — você recusou a execução da validação" in t and "a + b" in s.soma(), "C8: sem validação, não declara concluído", t)
        cid = t.split("Checkpoint: ")[1].split()[0]
        ck, _ = s.checkpoint(cid)
        checar(ck["editou"] and not ck["validou_depois_da_ultima_edicao"], "C8: checkpoint registra edição sem validação", ck)
        pr = s.perguntar("retome a tarefa de código %s" % cid)
        v = s.ate("plano")
        checar(pr and pr["dados"].get("codigo") and v and v["dados"]["intencao"] == "codigo_validacao", "C8: retomada vai direto à validação pendente",
               s.progresso()[-3:])
        s.decidir(v, "aprovar")
        r = s.ate("resposta", 60)
        checar(resposta_texto(r).startswith("Resposta: concluído"), "C8: retomada termina com a validação passando", resposta_texto(r))
        s.msgs = []
        pr = s.perguntar("retome a tarefa de código %s" % cid)
        checar(pr and "já foi concluída" in pr["dados"].get("resposta_texto", ""), "C8: retomar uma tarefa concluída só informa", pr and pr["dados"])
        pr = s.perguntar("retome a tarefa de código codigo-20990101-000000-0000")
        checar(pr and "Não encontrei" in pr["dados"].get("resposta_texto", ""), "C8: checkpoint inexistente", pr and pr["dados"])
        s.fim()

        print("== C9. cancelar durante a validação: processo encerrado, checkpoint, nada depois ==")
        s = Cenario(base, "c9", {"plano": PLANO, "edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        os.makedirs(os.path.join(s.proj, ".lia"))
        with open(os.path.join(s.proj, ".lia", "validacao.json"), "w") as f:
            json.dump({"comandos": {"lento": ["make", "lento"], "teste": ["make", "teste"]}}, f)
        s.perguntar("corrija o bug na função soma em soma.c e valide com lento")
        p = s.ate("plano")
        s.decidir(p, "aprovar")
        v = s.ate("plano")
        checar(v and "argv: [\"make\",\"lento\"]" in (v["dados"].get("previas") or [{}])[0].get("previa", ""), "C9: comando nomeado no pedido (e configurado) é usado")
        s.decidir(v, "aprovar")
        time.sleep(1.5)
        t0 = time.time()
        s.c.enviar("cancelar", {})
        res = s.ate("resultado", 10)
        depois = s.c.silencio(3)
        checar(res and res["dados"]["estado"] == "cancelado" and time.time() - t0 < 6 and not any(m["tipo"] == "resposta" for m in depois),
               "C9: cancelamento rápido e nada chega depois", res and res["dados"].get("estado"))
        sleeps = subprocess.run(["pgrep", "-f", "sleep 30"], capture_output=True, text=True).stdout.split()
        checar(not sleeps, "C9: o processo da validação foi encerrado", sleeps)
        ck_dir = os.path.join(s.home, ".lia", "projeto", "coding")
        cks = [json.load(open(os.path.join(ck_dir, f))) for f in os.listdir(ck_dir)]
        checar(cks and cks[0]["estado"] == "cancelado", "C9: checkpoint gravado como cancelado", cks)
        s.fim()

        def checkpoints(s):
            d = os.path.join(s.home, ".lia", "projeto", "coding")
            return [json.load(open(os.path.join(d, f))) for f in sorted(os.listdir(d))] if os.path.isdir(d) else []

        def log(s):
            return open(os.path.join(s.home, "sessiond.log")).read()

        print("== F1. travamento 'coletando dados 0/0': nada localizável -> o modelo escolhe da lista; nada válido -> encerra com motivo ==")
        s = Cenario(base, "f1a", {"contexto": {"arquivos": ["inexistente.c", "../fora.c"]}, "edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        pr = s.perguntar("corrija o bug da conta de adição do projeto")
        r = s.ate("resposta", 20)
        t = resposta_texto(r)
        checar(pr and pr["dados"].get("codigo") and t.startswith("Resposta: NÃO concluído — não localizei o código do pedido") and
               "Contexto lido: nenhum arquivo" in t and s.soma() == SOMA_BUG,
               "F1: sem contexto real a tarefa ENCERRA com motivo — nunca propõe sem ter lido nada, nunca fica parada", t)
        ck = checkpoints(s)
        checar(ck and ck[-1]["estado"] == "interrompido", "F1: checkpoint final coerente (interrompido)", ck)
        s.fim()
        s = Cenario(base, "f1b", {"contexto": {"arquivos": ["soma.c"]}, "edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        s.perguntar("corrija o bug da conta de adição do projeto")
        p = s.ate("plano", 20)
        prog = s.progresso()
        checar(p and p["dados"]["intencao"] == "codigo_edicao" and any("o modelo escolheu o contexto: soma.c" in x for x in prog) and
               any(x.startswith("li soma.c") for x in prog), "F1: contexto escolhido pelo modelo (conferido contra a lista) é lido antes da proposta", prog)
        ck = checkpoints(s)
        checar(ck and "soma.c" in ck[-1]["arquivos_lidos_sha256"], "F1: checkpoint gravado DEPOIS da leitura registra o que foi lido", ck and ck[-1])
        s.decidir(p, "rejeitar")
        s.ate("resposta")
        s.fim()

        print("== F2. critérios: absurdos do modelo descartados; erro do modelo -> critérios do pedido, o fluxo segue ==")
        absurdos = {"criterios": {"criterios": [{"descricao": "O código da função soma em soma.c está correto", "verificacao": "validacao", "texto": "return a * b * 0;"},
                                  {"descricao": "O arquivo sends.txt contém a linha correta", "verificacao": "contem", "caminho": "sends.txt", "texto": "|"},
                                  {"descricao": "calcularDesconto devolve zero", "verificacao": "validacao"},
                                  {"descricao": "soma(2,3) devolve 5 depois da correção do bug", "verificacao": "validacao"}]}}
        s = Cenario(base, "f2a", dict(absurdos, edicoes=[editar("a - b", "a + b")]), {"LIA_SESSIOND_CODIGO_CRITERIOS_MODELO": "1"})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c")
        p = s.ate("plano")
        s.decidir(p, "aprovar")
        v = s.ate("plano")
        s.decidir(v, "aprovar")
        r = s.ate("resposta", 60)
        t = resposta_texto(r)
        checar(t.startswith("Resposta: concluído") and "✓ soma(2,3) devolve 5 depois da correção do bug" in t, "F2: critério do modelo ligado ao pedido é aceito e conferido", t)
        checar("Critérios sugeridos e descartados pelo verificador:" in t and "return a * b * 0;" in t and "sends.txt" in t and "calcularDesconto" in t,
               "F2: critérios inventados (trecho, arquivo de dados, nome inexistente) aparecem como descartados, com motivo", t)
        s.fim()
        s = Cenario(base, "f2b", {"criterios": "isto não é JSON", "edicoes": [editar("a - b", "a + b")]}, {"LIA_SESSIOND_CODIGO_CRITERIOS_MODELO": "1"})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c")
        p = s.ate("plano")
        prog = s.progresso()
        checar(p and p["dados"]["intencao"] == "codigo_edicao" and any("não sugeriu critérios utilizáveis" in x and "sigo com os do pedido" in x for x in prog),
               "F2: erro do modelo nos critérios não interrompe: seguem os critérios do pedido", prog)
        s.decidir(p, "rejeitar")
        s.ate("resposta")
        s.fim()

        print("== F3. prazo do modelo, cancelamento e desconexão durante a proposta ==")
        s = Cenario(base, "f3a", {"edicoes": [editar("a - b", "a + b")]}, {"LIA_SESSIOND_CODIGO_TIMEOUT_MS": "1500"})
        cenarios.append(s)
        t0 = time.time()
        s.perguntar("corrija o bug na função soma em soma.c MOCK_CODIGO_LENTO")
        r = s.ate("resposta", 15)
        t = resposta_texto(r)
        checar(t.startswith("Resposta: NÃO concluído — o modelo local (mock) não respondeu em 1 s") and time.time() - t0 < 10,
               "F3: prazo estourado ENCERRA a tarefa com o motivo (nada fica ativo)", t[:300])
        ck = checkpoints(s)
        checar(ck and ck[-1]["estado"] == "interrompido" and not s.c.silencio(2), "F3: checkpoint 'interrompido' e nada chega depois", ck and ck[-1]["estado"])
        pr = s.perguntar("corrija o bug na função soma em soma.c")
        checar(pr and pr["dados"].get("codigo"), "F3: a conexão aceita uma nova tarefa depois do prazo (nada ficou preso)", pr and pr["dados"])
        s.fim()
        s = Cenario(base, "f3b", {"edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c MOCK_CODIGO_LENTO")
        time.sleep(1)
        s.c.enviar("cancelar", {})
        e = s.ate("erro", 10)
        depois = s.c.silencio(2)
        ck = checkpoints(s)
        checar(e and e["dados"].get("codigo") == "cancelado" and not any(m["tipo"] == "resposta" for m in depois) and ck and ck[-1]["estado"] == "cancelado",
               "F3: 'cancelar' durante a proposta: confirmação, checkpoint 'cancelado', nada depois", (e, ck and ck[-1]["estado"]))
        s.fim()
        s = Cenario(base, "f3c", {"edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        s.perguntar("corrija o bug na função soma em soma.c MOCK_CODIGO_LENTO")
        time.sleep(1)
        s.c.fechar()
        time.sleep(1)
        ck = checkpoints(s)
        checar(ck and ck[-1]["estado"] == "desconectado" and "o cliente desconectou" in log(s), "F3: desconexão encerra a tarefa com checkpoint 'desconectado'",
               (ck and ck[-1]["estado"], log(s)[-300:]))
        s.c = Conexao(s.home)
        pr = s.perguntar("retome a tarefa de código %s" % ck[-1]["id"])
        s.msgs += s.c.silencio(1.5)
        prog = s.progresso()
        checar(pr and pr["dados"].get("codigo") and any("próxima etapa: nova proposta" in x for x in prog) and any("pedindo ao modelo a alteração" in x for x in prog),
               "F3: a tarefa desconectada é retomada pelo checkpoint (volta à proposta)", prog[-3:])
        s.c.enviar("cancelar", {})
        s.ate("erro", 10)
        s.fim()

        print("== F4. caminhos em instruções negativas nunca são lidos nem alterados ==")
        s = Cenario(base, "f4", {"edicoes": [editar("int main", "int principal", "teste_soma.c"), editar("a - b", "a + b")]})
        cenarios.append(s)
        with open(os.path.join(s.proj, "sends.txt"), "w") as f:
            f.write("DADO-REAL|x\n")
        s.perguntar("corrija o bug na função soma em soma.c sem alterar o formato de sends.txt. Não leia nem altere teste_soma.c, não leia .env")
        p = s.ate("plano")
        prog = s.progresso()
        checar(not any(x.startswith("li teste_soma.c") or x.startswith("li sends.txt") or x.startswith("li .env") for x in prog) and
               any("o pedido proíbe tocar em:" in x and "sends.txt" in x and "teste_soma.c" in x and ".env" in x for x in prog),
               "F4: teste_soma.c, sends.txt e .env (citados em 'sem alterar'/'não leia') não são lidos; o progresso diz por quê", prog)
        checar(any("proposta recusada pelo verificador" in x and "não foi lido" in x for x in prog), "F4: proposta que mexe no arquivo negado é recusada", prog)
        s.decidir(p, "aprovar")
        v = s.ate("plano")
        s.decidir(v, "aprovar")
        r = s.ate("resposta", 60)
        t = resposta_texto(r)
        checar(t.startswith("Resposta: concluído") and "✓ nada que o pedido proíbe foi lido ou alterado" in t and "Contexto lido: soma.c\n" in t and
               "DADO-REAL" not in json.dumps(s.msgs), "F4: concluído; critério de proibição conferido; nenhum byte do arquivo de dados saiu", t)
        s.fim()

        print("== F4b. a validação (código do projeto) mexe num caminho proibido: nunca 'concluído' ==")
        s = Cenario(base, "f4b", {"edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        with open(os.path.join(s.proj, "Makefile"), "a") as f:
            f.write("teste-log: teste_soma\n\t./teste_soma\n\techo envio >> registro.log\n")
        os.makedirs(os.path.join(s.proj, ".lia"))
        with open(os.path.join(s.proj, ".lia", "validacao.json"), "w") as f:
            json.dump({"comandos": {"teste-log": ["make", "teste-log"], "teste": ["make", "teste"]}}, f)
        s.perguntar("corrija o bug na função soma em soma.c e valide com teste-log. Não altere logs.")
        p = s.ate("plano")
        s.decidir(p, "aprovar")
        v = s.ate("plano")
        s.decidir(v, "aprovar")
        r = s.ate("resposta", 60)
        t = resposta_texto(r)
        checar(t.startswith("Resposta: NÃO concluído — a validação (código do projeto/dos testes) mexeu em caminho que o pedido proíbe: registro.log (criado)"),
               "F4b: escrita da validação em caminho proibido é detectada e impede 'concluído'", t[:400])
        s.fim()

        print("== F5. roteamento: pedido de código nunca cai em arquivo.ler/anexar ==")
        s = Cenario(base, "f5", {"edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        for pedido in ("Leia soma.c e corrija a função soma", "Altere soma.c para que soma some de verdade; nao leia .env",
                       "Em soma.c, ajuste soma e adicione um teste para cobri-la. Mantenha a compatibilidade e preserve o conteúdo do resto."):
            pr = s.perguntar(pedido)
            checar(pr and pr["dados"].get("codigo") is True, "F5: '%s' -> ciclo de coding" % pedido[:50], pr and pr["dados"].get("plano_proposto"))
            p = s.ate("plano")
            if p:
                s.decidir(p, "rejeitar")
            s.ate("resposta")
        with open(os.path.join(s.proj, "notas.txt"), "w") as f:
            f.write("notas\n")
        pr = s.perguntar("acrescente ao final de notas.txt o texto: revisar soma")
        pl = pr and pr["dados"].get("plano_proposto")
        checar(pl and pl["passos"][0]["ferramenta"] == "arquivo.anexar" and not pr["dados"].get("codigo"), "F5: texto literal continua operação direta de arquivo", pr and pr["dados"])
        pr = s.perguntar("acrescente ao fim de notas.txt uma explicação sobre o projeto")
        checar(pr and not pr["dados"].get("plano_proposto") and not pr["dados"].get("codigo"), "F5: escrita sem conteúdo literal -> pergunta, nada gravado",
               pr and pr["dados"].get("resposta_texto"))
        s.fim()

        print("== V. 'rode os testes' e 'git' fora do ciclo ==")
        s = Cenario(base, "v", {})
        cenarios.append(s)
        pr = s.perguntar("rode os testes")
        pl = pr["dados"]["plano_proposto"]
        s.c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": pr["dados"]["pergunta_id"]})
        p = s.ate("plano")
        checar(p and p["dados"]["requer_aprovacao"] is True and "executável: /usr/bin/make" in (p["dados"].get("previas") or [{}])[0].get("previa", ""),
               "V: 'rode os testes' pede aprovação com a prévia do comando", p and p["dados"])
        s.decidir(p, "aprovar")
        r = s.ate("resposta", 60)
        checar(resposta_texto(r).startswith("A validação \"teste\" falhou (código de saída 2)"), "V: resposta honesta (o teste falha antes da correção)", resposta_texto(r))
        logs = "".join(open(os.path.join(s.home, ".lia", "humano", "auditoria", f)).read() for f in os.listdir(os.path.join(s.home, ".lia", "humano", "auditoria")))
        checar("acao=sessiond.validar.executar" in logs and "comando=teste passou=nao codigo_saida=2" in logs and "FALHA soma" not in logs,
               "V: auditoria com comando e código de saída, sem a saída", logs[-400:])
        # git status (leitura, automático) e commit com aprovação
        with open(os.path.join(s.proj, "soma.c"), "w") as f:
            f.write("int soma(int a, int b) { return a + b; }\n")
        with open(os.path.join(s.proj, ".git", "hooks", "pre-commit"), "w") as f:
            f.write("#!/bin/sh\ntouch GANCHO-RODOU\n")
        os.chmod(os.path.join(s.proj, ".git", "hooks", "pre-commit"), 0o755)
        pr = s.perguntar("git status")
        pl = pr["dados"]["plano_proposto"]
        checar(pr["dados"]["validacao"].get("execucao_automatica_permitida") is True, "V: git status é leitura automática")
        s.c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": pr["dados"]["pergunta_id"]})
        s.ate("resultado")
        r = s.ate("resposta")
        checar("principal" in resposta_texto(r) and "1 arquivo(s)" in resposta_texto(r), "V: git status responde ramo e alterações", resposta_texto(r))
        pr = s.perguntar("faça commit de soma.c com a mensagem \"Corrige a soma\"")
        pl = pr["dados"]["plano_proposto"]
        s.c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": pr["dados"]["pergunta_id"]})
        p = s.ate("plano")
        pc = (p["dados"].get("previas") or [{}])[0].get("previa", "")
        checar("│ Corrige a soma" in pc and " M soma.c" in pc and "ganchos do Git desativados" in pc, "V: commit pede aprovação com mensagem, arquivos e diff", pc)
        s.decidir(p, "rejeitar")
        res = s.ate("resultado")
        r = s.ate("resposta")
        checar(res["dados"].get("resposta_a_seguir") and "Nada foi feito" in resposta_texto(r) and "inicial" in git(s.proj, "log", "-1", "--format=%s"),
               "V: commit recusado: nada commitado, resposta final", resposta_texto(r))
        pr = s.perguntar("faça commit de soma.c com a mensagem \"Corrige a soma\"")
        pl = pr["dados"]["plano_proposto"]
        s.c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": pr["dados"]["pergunta_id"]})
        p = s.ate("plano")
        s.decidir(p, "aprovar")
        r = s.ate("resposta", 30)
        checar("Commit" in resposta_texto(r) and "nada foi enviado" in resposta_texto(r) and
               git(s.proj, "log", "-1", "--format=%an|%s").strip() == "Lia Teste|Corrige a soma" and not os.path.exists(os.path.join(s.proj, "GANCHO-RODOU")),
               "V: commit local criado, autor certo, gancho do repositório não rodou", resposta_texto(r))
        s.fim()

        print("== H. lia-chat: tarefa de código inteira respondendo 's' ==")
        s = Cenario(base, "h", {"plano": PLANO, "edicoes": [editar("a - b", "a + b")]})
        cenarios.append(s)
        s.c.fechar()
        chat = subprocess.Popen([CHAT], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                env={"HOME": s.home, "PATH": os.environ.get("PATH", "/usr/bin:/bin")})
        chat.stdin.write("corrija o bug na função soma em soma.c\n")
        chat.stdin.flush()
        time.sleep(3)
        chat.stdin.write("s\n")
        chat.stdin.flush()
        time.sleep(4)
        chat.stdin.write("s\n")
        chat.stdin.flush()
        time.sleep(4)
        chat.stdin.close()
        saida = chat.stdout.read()
        chat.wait(timeout=15)
        checar("[código]" in saida and "[prévia] +int soma(int a, int b) { return a + b; }" in saida and "[prévia] executar a validação \"teste\"" in saida,
               "H: chat mostra etapas, a prévia da edição e a do comando", saida[-2500:])
        checar("Resposta: concluído" in saida and "a + b" in s.soma(), "H: chat chega ao relatório final", saida[-1500:])
        parar(s.d)
    finally:
        for s in cenarios:
            parar(s.d)
    restos = [p for p in subprocess.run(["pgrep", "-f", base], capture_output=True, text=True).stdout.split() if p != str(os.getpid())]
    checar(not restos, "nenhum processo residual", restos)
    if poc_antes is not None:
        poc_depois = subprocess.run(["git", "-C", "/home/zad/poc", "status", "--porcelain"], capture_output=True, text=True).stdout
        checar(poc_antes == poc_depois, "/home/zad/poc não foi alterado")
    if not os.environ.get("LIA_DEBUG_MANTER"):
        shutil.rmtree(base, ignore_errors=True)
    print("\n%d testes, %d falhas" % (total, falhas))
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
