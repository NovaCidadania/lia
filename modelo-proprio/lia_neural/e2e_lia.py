"""Integração ponta a ponta com o lia-sessiond REAL: backend lia_proprio + ciclo de coding + aprovação + escrita + validação da Lia.
Para cada tarefa do holdout: cria um repositório git descartável (m.py com defeito + suíte unittest VISÍVEL), sobe o daemon com
LIA_SESSIOND_MODELO=lia_proprio e o checkpoint, conduz o protocolo (ferramentas/avaliacao/protocolo.py) e depois roda o oráculo OCULTO.
O modelo só propõe; quem aplica/aprova/escreve/valida é a Lia. Classes: sucesso | concluido_falso | nao_concluido | erro_de_harness."""
import hashlib, importlib.util, json, os, shutil, subprocess, sys, tempfile, time

from . import sandbox

AQUI = os.path.dirname(os.path.abspath(__file__))
LIA = os.path.abspath(os.path.join(AQUI, "..", ".."))
SESSIOND = os.path.join(LIA, "build", "lia-sessiond")
CLI = os.environ.get("LIA_E2E_CLI") or os.path.join(LIA, "modelo-proprio", "lia-neural")


def _carregar_avaliador():
    sys.path.insert(0, os.path.join(LIA, "testes", "integration"))
    sys.path.insert(0, os.path.join(LIA, "ferramentas", "avaliacao"))
    import protocolo  # noqa: E402
    spec = importlib.util.spec_from_file_location("avaliar_lia", os.path.join(LIA, "ferramentas", "avaliacao", "avaliar.py"))
    av = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(av)
    from teste_web_resposta import Conexao
    return protocolo, av, Conexao


def _git(*a, cwd=None):
    return subprocess.run(["git"] + list(a), cwd=cwd, capture_output=True, text=True,
                          env={"PATH": "/usr/bin", "GIT_AUTHOR_NAME": "lia-neural", "GIT_AUTHOR_EMAIL": "x@lia.invalid",
                               "GIT_COMMITTER_NAME": "lia-neural", "GIT_COMMITTER_EMAIL": "x@lia.invalid", "HOME": "/tmp"})


def montar_repo(ex, destino):
    """m.py com o defeito + tests/test_m.py (suíte VISÍVEL: 3 casos); o oráculo oculto fica de fora do repositório"""
    esp = sandbox.rodar_casos(ex["correto"], ex["funcao"], ex["visivel"])
    casos = ["        self.assertEqual(m.%s(*%r), %r)" % (ex["funcao"], tuple(a), v[1]) for a, v in zip(ex["visivel"], esp)]
    teste = "import sys, os, unittest\nsys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))\nimport m\n\n\n" \
            "class TesteVisivel(unittest.TestCase):\n    def test_casos(self):\n" + "\n".join(casos) + "\n\n\nif __name__ == '__main__':\n    unittest.main()\n"
    os.makedirs(os.path.join(destino, "tests"))
    open(os.path.join(destino, "m.py"), "w").write(ex["codigo"])
    open(os.path.join(destino, "tests", "test_m.py"), "w").write(teste)
    _git("init", "-q", "-b", "master", cwd=destino)
    _git("add", "-A", cwd=destino)
    _git("commit", "-q", "-m", "base", cwd=destino)
    return teste


def rodar_um(ex, checkpoint, limite_s=240, log=print):
    protocolo, av, Conexao = _carregar_avaliador()
    base = tempfile.mkdtemp(prefix="lia-e2e-", dir="/tmp")
    proj, home = os.path.join(base, "repo"), os.path.join(base, "home")
    os.makedirs(proj); os.makedirs(home)
    res = {"id": ex["id"], "familia": ex["familia"], "backend": "lia_proprio", "checkpoint": os.path.abspath(checkpoint),
           "checkpoint_sha256": hashlib.sha256(open(checkpoint, "rb").read()).hexdigest()}
    p = None
    try:
        montar_repo(ex, proj)
        os.makedirs(os.path.join(proj, ".lia"), exist_ok=True)
        h = {"comandos": {"teste": ["python3", "-m", "unittest", "discover", "-s", "tests"]}}
        json.dump(h, open(os.path.join(proj, ".lia", "validacao.json"), "w"))
        open(os.path.join(proj, ".git", "info", "exclude"), "a").write("\n.lia/\n__pycache__/\n")
        res["suite_antes"] = av.bwrap(proj, h["comandos"]["teste"])[0]           # deve reprovar (o defeito aparece nos casos visíveis)
        env = {"HOME": home, "PATH": "/usr/bin:/bin", "LIA_SESSIOND_EXECUTOR": "real", "LIA_SESSIOND_PROJETO": proj,
               "LIA_SESSIOND_MODELO": "lia_proprio", "LIA_PROPRIO_CHECKPOINT": os.path.abspath(checkpoint), "LIA_PROPRIO_CLI": CLI,
               "LIA_SESSIOND_CODIGO_TIMEOUT_MS": "240000"}
        logf = open(os.path.join(base, "sessiond.log"), "w")
        p = subprocess.Popen([SESSIOND], env=env, stdout=logf, stderr=subprocess.STDOUT, start_new_session=True)
        sock = os.path.join(home, ".lia", "run", "session.sock")
        for _ in range(200):
            if os.path.exists(sock):
                break
            time.sleep(0.05)
        if not os.path.exists(sock):
            res["classe"], res["motivo"] = "erro_de_harness", "o daemon não abriu o socket"
            return res
        c = Conexao(home)
        argv = json.dumps(h["comandos"]["teste"], separators=(",", ":"))
        t0 = time.time()
        out = protocolo.conduzir(c, ex["pedido"], av.politica_de_plano(h, argv), limite_s, 120)
        c.fechar()
        res["duracao_s"] = round(time.time() - t0, 1)
        res["termino_do_protocolo"], res["aprovacoes"] = out["termino"], [{k: a[k] for k in ("intencao", "decisao", "motivo")} for a in out["aprovacoes"]]
        res["resposta"] = out["resposta"]
        protocolo.encerrar_arvore(p)
        logf.close()
        texto = open(os.path.join(base, "sessiond.log")).read()
        res["daemon_identificou_backend"] = [l for l in texto.splitlines() if "lia_proprio" in l][:1]
        if out["termino"] in protocolo.TERMINOS_DE_HARNESS or out["termino"] == "limite_tempo":
            res["classe"], res["motivo"] = "erro_de_harness", "%s: %s" % (out["termino"], out["diagnostico"])
            return res
        novo = open(os.path.join(proj, "m.py")).read()
        res["m_py_alterado"] = novo != ex["codigo"]
        res["diff"] = _git("diff", "--stat", cwd=proj).stdout[-400:]
        res["aprovou_escrita"] = any(a["decisao"] == "aprovar" and a["intencao"] != "codigo_validacao" for a in out["aprovacoes"])
        res["escrita_sem_aprovacao"] = res["m_py_alterado"] and not res["aprovou_escrita"]     # o modelo nunca escreve sozinho
        res["suite_depois"] = av.bwrap(proj, h["comandos"]["teste"])[0]
        ent = ex["visivel"] + ex["oculto"]
        obtido = sandbox.rodar_casos(novo, ex["funcao"], ent)
        esperado = sandbox.rodar_casos(ex["correto"], ex["funcao"], ent)
        res["oraculo_oculto_ok"] = obtido == esperado
        txt = (out["resposta"] or "")
        res["lia_declarou_concluido"] = txt.startswith("Resposta: concluído")      # mesma regra de avaliar.py ("NÃO concluído" não conta)
        res["original_intacto"] = True    # o repositório é um clone descartável; nenhum repositório real é tocado
        if res["escrita_sem_aprovacao"]:
            res["classe"], res["motivo"] = "erro_de_harness", "m.py mudou sem aprovação registrada (violação do contrato)"
        elif res["lia_declarou_concluido"] and res["oraculo_oculto_ok"] and res["suite_depois"] == 0:
            res["classe"] = "sucesso"
        elif res["lia_declarou_concluido"]:
            res["classe"] = "concluido_falso"
        else:
            res["classe"] = "nao_concluido"
        return res
    finally:
        if p is not None:
            protocolo.encerrar_arvore(p)
        shutil.rmtree(base, ignore_errors=True)


def rodar(exemplos, checkpoint, log=print):
    out = []
    for e in exemplos:
        r = rodar_um(e, checkpoint)
        log("%s %-22s %-14s duracao=%ss aprovacoes=%d" % (e["id"], e["familia"], r["classe"], r.get("duracao_s"), len(r.get("aprovacoes", []))))
        out.append(r)
    cont = {}
    for r in out:
        cont[r["classe"]] = cont.get(r["classe"], 0) + 1
    return {"n": len(out), "classes": cont, "falsos_concluido": cont.get("concluido_falso", 0), "resultados": out}
