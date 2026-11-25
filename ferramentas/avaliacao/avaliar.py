#!/usr/bin/env python3
"""Avaliação real da Lia em tarefas congeladas — uma execução por chamada.

  avaliar.py --tarefas ARQ.json --id ID --condicao COND --rep N --saida DIR

COND:
  sem-modelo                 só o runtime (receitas determinísticas + encerramento honesto)
  ollama:<modelo>            ex.: ollama:llama3.2:3b
  llamacpp:<porta>:<rotulo>  llama-server já rodando em 127.0.0.1:<porta> (formato chatml)

Cada execução: clone temporário (só arquivos versionados — .env e dados reais
não existem no clone; subdiretório que é repositório próprio é clonado
direto), commit-base registrado, harness de validação fora do diff (.lia/),
lia-sessiond real em HOME isolado, aprovação por política fixa (registrada),
depois revisão independente em bubblewrap: verificação OCULTA da tarefa e a
suíte do projeto. O repositório original é conferido antes e depois.
Nunca instala nada, nunca liga banco, nunca roda migrations."""
import argparse, atexit, hashlib, json, os, shutil, subprocess, sys, time

AQUI = os.path.dirname(os.path.abspath(__file__))
LIA = os.path.abspath(os.path.join(AQUI, "..", ".."))
sys.path.insert(0, os.path.join(LIA, "testes", "integration"))
from teste_web_resposta import Conexao, parar  # noqa: E402

SESSIOND = os.path.join(LIA, "build", "lia-sessiond")
BASE = "/tmp/lia-aval"  # caminho curto: o socket AF_UNIX tem limite de 108 bytes

# harness de validação por repositório: só o que JÁ existe nesta máquina
HARNESS = {
    "sf": {"comandos": {"teste": ["python3", "-m", "unittest", "discover", "-s", "tests", "-v"]}},
    "le": {"comandos": {"teste": ["node", "node_modules/vitest/vitest.mjs", "run", "--config", ".lia/vitest.unidade.config.mjs"]}},
}
VITEST_UNIDADE = """// harness da avaliação (fora do repositório, em .lia/ — oculto para a Lia): só testes
// unitários, sem o globalSetup que roda 'prisma migrate deploy' (nunca rodar migrations)
import { defineConfig } from 'vitest/config'
export default defineConfig({ test: { include: ['src/**/*.test.ts'], environment: 'node', env: { NODE_ENV: 'test' } } })
"""
VITEST_OCULTA = """import { defineConfig } from 'vitest/config'
export default defineConfig({ test: { include: ['.lia/oculta.test.ts'], environment: 'node', env: { NODE_ENV: 'test' } } })
"""


def sh(*a, cwd=None, timeout=600):
    return subprocess.run(list(a), cwd=cwd, capture_output=True, text=True, timeout=timeout)


def bwrap(proj, argv, extra_env=None, timeout=300):
    """revisão independente: sem rede, /home invisível, só o clone gravável"""
    cmd = ["/usr/bin/bwrap", "--unshare-all", "--die-with-parent", "--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin",
           "--symlink", "usr/lib", "/lib", "--symlink", "usr/lib64", "/lib64", "--ro-bind", "/etc", "/etc", "--proc", "/proc",
           "--dev", "/dev", "--tmpfs", "/tmp", "--bind", proj, proj, "--chdir", proj, "--clearenv",
           "--setenv", "HOME", "/tmp", "--setenv", "PATH", "/usr/bin"]
    for k, v in (extra_env or {}).items():
        cmd += ["--setenv", k, v]
    try:
        r = subprocess.run(cmd + argv, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr)[-4000:]
    except subprocess.TimeoutExpired:
        return 124, "tempo esgotado"


def chave_repo(projeto):
    return os.path.basename(projeto.rstrip("/"))


def carregar_oculta(arquivo_tarefas, tid, ref):
    """O código da verificação oculta vive fora do repositório (~/.lia/avaliacao-oculta/, 0600);
    o arquivo de tarefas só traz tipo e sha256, que precisam bater."""
    caminho = os.path.join(os.path.expanduser("~/.lia/avaliacao-oculta"), os.path.basename(arquivo_tarefas))
    try:
        c = json.load(open(caminho))[tid]
    except (OSError, KeyError, ValueError):
        sys.exit("verificação oculta de %s ausente em %s: tarefa não avaliável" % (tid, caminho))
    sha = hashlib.sha256(c["codigo"].encode()).hexdigest()
    if sha != ref.get("sha256"):
        sys.exit("verificação oculta de %s não confere com o hash fixado antes da avaliação" % tid)
    return {"tipo": c["tipo"], "codigo": c["codigo"], "sha256": sha}


def redigir_tarefa(t):
    t = dict(t)
    o = t.get("verificacao_oculta")
    if o and "codigo" in o:
        t["verificacao_oculta"] = {"tipo": o["tipo"], "sha256": hashlib.sha256(o["codigo"].encode()).hexdigest()}
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tarefas", required=True)
    ap.add_argument("--id", required=True)
    ap.add_argument("--condicao", required=True)
    ap.add_argument("--rep", type=int, default=1)
    ap.add_argument("--saida", required=True)
    ap.add_argument("--modelo-hash", default="")
    ap.add_argument("--limite-min", type=int, default=40)
    ap.add_argument("--temperatura", default="")   # vazio = padrão da Lia (0)
    ap.add_argument("--semente", default="")
    ap.add_argument("--sem-portao-arvore", action="store_true", help="ablação: loop simples (a árvore registra, mas não decide o concluído)")
    a = ap.parse_args()
    doc = json.load(open(a.tarefas))
    t = dict(next(x for x in doc["tarefas"] if x["id"] == a.id))
    if t.get("verificacao_oculta") and "codigo" not in t["verificacao_oculta"]:
        t["verificacao_oculta"] = carregar_oculta(a.tarefas, t["id"], t["verificacao_oculta"])  # antes de rodar: falha cedo
    rotulo = "%s__%s__r%d%s" % (a.id, a.condicao.replace(":", "_").replace("/", "_"), a.rep, ("_T" + a.temperatura) if a.temperatura else "") + ("_semarvore" if a.sem_portao_arvore else "")
    d = os.path.join(BASE, hashlib.sha256(rotulo.encode()).hexdigest()[:10])
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    clone, home = os.path.join(d, "repo"), os.path.join(d, "home")
    os.makedirs(home)
    origem, subdir = t["projeto"], t.get("subdir", "")
    if subdir and os.path.isdir(os.path.join(origem, subdir, ".git")):
        origem, subdir = os.path.join(origem, subdir), ""
    orig_status_antes = sh("git", "-C", origem, "status", "--porcelain").stdout
    orig_head = sh("git", "-C", origem, "rev-parse", "HEAD").stdout.strip()
    r = sh("git", "clone", "--no-hardlinks", "--quiet", origem, clone)
    if r.returncode:
        print(r.stderr)
        return 2
    proj = os.path.join(clone, subdir) if subdir else clone
    tarefas_sha = hashlib.sha256(open(a.tarefas, "rb").read()).hexdigest()
    res = {"formato": "lia-avaliacao-execucao/2", "tarefa": redigir_tarefa(t), "tarefas_arquivo": os.path.relpath(a.tarefas, LIA), "tarefas_sha256": tarefas_sha,
           "condicao": a.condicao, "rep": a.rep, "modelo_hash": a.modelo_hash, "repositorio_clonado": origem,
           "commit_base": sh("git", "-C", clone, "rev-parse", "HEAD").stdout.strip(), "commit_original": orig_head,
           "lia_sessiond_sha256": hashlib.sha256(open(SESSIOND, "rb").read()).hexdigest(), "eventos": [], "aprovacoes": []}
    h = HARNESS.get(chave_repo(t["projeto"]))
    os.makedirs(os.path.join(proj, ".lia"), exist_ok=True)
    if h:
        json.dump(h, open(os.path.join(proj, ".lia", "validacao.json"), "w"))
        if chave_repo(t["projeto"]) == "le":
            shutil.copytree(os.path.join(origem, "node_modules"), os.path.join(proj, "node_modules"), symlinks=True)
            open(os.path.join(proj, ".lia", "vitest.unidade.config.mjs"), "w").write(VITEST_UNIDADE)
    res["harness"] = h or "bloqueado por ambiente (sem validação executável nesta máquina)"
    with open(os.path.join(clone, ".git", "info", "exclude"), "a") as f:
        f.write("\n.lia/\nnode_modules/\n__pycache__/\n")
    # suíte do projeto ANTES (base para detectar regressão)
    if h:
        res["suite_antes"] = bwrap(proj, h["comandos"]["teste"])[0]
    env = {"HOME": home, "PATH": "/usr/bin:/bin", "LIA_SESSIOND_EXECUTOR": "real", "LIA_SESSIOND_PROJETO": proj,
           "LIA_SESSIOND_CODIGO_TIMEOUT_MS": "900000"}
    cond = a.condicao.split(":")
    if cond[0] == "sem-modelo":
        env.update({"LIA_SESSIOND_MODELO": "mock"})  # o mock sem roteiro não propõe edição nenhuma
    elif cond[0] == "ollama":
        env.update({"LIA_SESSIOND_MODELO": "ollama", "LIA_OLLAMA_MODELO": ":".join(cond[1:]), "LIA_OLLAMA_TIMEOUT_MS": "600000"})
    elif cond[0] == "llamacpp":
        env.update({"LIA_SESSIOND_MODELO": "llama_cpp", "LIA_LLAMACPP_PORTA": cond[1], "LIA_LLAMACPP_FORMATO": "chatml",
                    "LIA_LLAMACPP_TIMEOUT_MS": "600000"})
    if a.temperatura:
        env["LIA_MODELO_TEMPERATURA"] = a.temperatura
    if a.semente:
        env["LIA_MODELO_SEMENTE"] = a.semente
    if a.sem_portao_arvore:
        env["LIA_SESSIOND_ARVORE_PORTAO"] = "0"
    res["parametros"] = {k: v for k, v in env.items() if k.startswith("LIA_") and k != "LIA_SESSIOND_PROJETO"}
    log = open(os.path.join(d, "sessiond.log"), "w")
    p = subprocess.Popen([SESSIOND], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    atexit.register(lambda: p.poll() is None and parar(p))
    sock = os.path.join(home, ".lia", "run", "session.sock")
    for _ in range(200):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    c = Conexao(home)
    t0 = time.time()
    res["inicio"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    c.enviar("perguntar", {"mensagem": t["prompt"]})
    fim, resposta = time.time() + a.limite_min * 60, None
    argv_harness = json.dumps((h or {}).get("comandos", {}).get("teste", []), separators=(",", ":"))
    while time.time() < fim and resposta is None:
        msgs, _ = c.ate("__nada__", 5)
        for m in msgs:
            ev = {"t": round(time.time() - t0, 1), "tipo": m["tipo"]}
            dd = m.get("dados", {})
            if m["tipo"] == "progresso":
                ev["mensagem"] = dd.get("mensagem")
                if "codigo" in dd:
                    ev["etapa"] = dd["codigo"].get("etapa")
            elif m["tipo"] == "plano":
                ev["intencao"] = dd.get("intencao")
                passos = [{"ferramenta": x.get("ferramenta"), "caminho": (x.get("parametros") or {}).get("caminho")} for x in dd.get("passos", [])]
                previas = "\n".join(x.get("previa") or "" for x in (dd.get("previas") or []))
                decisao, motivo = "aprovar", ""
                if dd.get("intencao") == "codigo_validacao":
                    if not h or ("argv: " + argv_harness) not in previas:
                        decisao, motivo = "rejeitar", "argv diferente do harness"
                else:
                    for x in passos:
                        cam = x.get("caminho") or ""
                        if cam.startswith("/") or ".." in cam.split("/") or cam.endswith(".meta") or cam.startswith(".env") or cam.startswith(".lia"):
                            decisao, motivo = "rejeitar", "caminho fora do escopo: " + cam
                res["aprovacoes"].append({"t": ev["t"], "intencao": dd.get("intencao"), "decisao": decisao, "motivo": motivo, "passos": passos})
                c.enviar("aprovar", {"plano_id": dd["plano_id"], "hash": dd["hash"], "decisao": decisao, "escopo": "uma_vez"})
            elif m["tipo"] == "resposta":
                resposta = dd.get("resposta_texto")
            elif m["tipo"] == "proposta" and not dd.get("codigo") and not dd.get("plano_proposto"):
                resposta = dd.get("resposta_texto") or "(proposta sem tarefa de código)"
            res["eventos"].append(ev)
    res["duracao_s"] = round(time.time() - t0, 1)
    res["resposta"] = resposta if resposta is not None else "(sem resposta final no limite)"
    c.fechar()
    parar(p)
    log.close()
    texto_log = open(os.path.join(d, "sessiond.log")).read()
    res["log_daemon"] = texto_log[-8000:]
    import re
    chamadas = re.findall(r"prompt (-?\d+) tokens em (-?\d+) ms · geração (-?\d+) tokens em (-?\d+) ms", texto_log)
    res["modelo_chamadas"] = len(chamadas)
    res["tokens_prompt"] = sum(max(int(x[0]), 0) for x in chamadas)
    res["tokens_gerados"] = sum(max(int(x[2]), 0) for x in chamadas)
    # árvore e checkpoint da tarefa
    cdir = os.path.join(home, ".lia", "projeto", "coding")
    cks = sorted(f for f in os.listdir(cdir) if f.endswith(".json")) if os.path.isdir(cdir) else []
    ck = json.load(open(os.path.join(cdir, cks[-1]))) if cks else {}
    res["checkpoint"] = {k: ck.get(k) for k in ("id", "estado", "causa", "truncamentos", "uma_edicao_por_chamada", "ciclo", "editou",
                                                "validou_depois_da_ultima_edicao", "validacao_passou")}
    arv_p = os.path.join(cdir, ck.get("id", "_"), "arvore.json")
    res["arvore"] = json.load(open(arv_p)) if os.path.exists(arv_p) else None
    tents = [x for n in (res["arvore"] or {}).get("nos", []) for x in n.get("tentativas", [])]
    res["tentativas"] = len(tents)
    res["tentativas_validas"] = sum(1 for x in tents if (x.get("resultado") or "").startswith(("passou no verificador", "aplicada", "rejeitada pela pessoa")))
    res["tentativas_truncadas"] = sum(1 for x in tents if "truncamento" in (x.get("resultado") or ""))
    res["git_status"] = sh("git", "-C", clone, "status", "--porcelain").stdout
    res["git_diff"] = sh("git", "-C", clone, "diff").stdout
    novos = sh("git", "-C", clone, "ls-files", "--others", "--exclude-standard").stdout.split()
    res["arquivos_novos"] = {n: open(os.path.join(clone, n), errors="replace").read() for n in novos
                             if os.path.isfile(os.path.join(clone, n)) and os.path.getsize(os.path.join(clone, n)) < 200000}
    concluido = (res["resposta"] or "").startswith("Resposta: concluído")
    res["concluido"] = concluido
    # revisão independente (fora da Lia): verificação oculta + suíte do projeto
    oc = t.get("verificacao_oculta")
    houve_diff = bool(res["git_diff"].strip() or novos)
    res["oculta"] = None
    if oc and houve_diff and h:
        if oc["tipo"] == "python":
            open(os.path.join(proj, ".lia", "oculta.py"), "w").write(oc["codigo"])
            rc, out = bwrap(proj, ["python3", ".lia/oculta.py"], {"PYTHONPATH": proj})
        else:
            open(os.path.join(proj, ".lia", "oculta.test.ts"), "w").write(oc["codigo"])
            open(os.path.join(proj, ".lia", "vitest.oculta.config.mjs"), "w").write(VITEST_OCULTA)
            rc, out = bwrap(proj, ["node", "node_modules/vitest/vitest.mjs", "run", "--config", ".lia/vitest.oculta.config.mjs"])
        res["oculta"] = {"passou": rc == 0, "codigo_saida": rc, "saida": out[-300:], "saida_sha256": hashlib.sha256(out.encode()).hexdigest(), "oraculo_sha256": oc.get("sha256")}
    res["suite_depois"] = bwrap(proj, h["comandos"]["teste"])[0] if (h and houve_diff) else None
    status_depois = sh("git", "-C", origem, "status", "--porcelain").stdout
    res["original_intocado"] = status_depois == orig_status_antes and sh("git", "-C", origem, "rev-parse", "HEAD").stdout.strip() == orig_head
    proib_violada = any("proíbe" in (e.get("mensagem") or "") and "mexeu" in (e.get("mensagem") or "") for e in res["eventos"])
    res["violacao_proibicao"] = proib_violada or "validacao_proibidos" == (ck.get("causa") or "")
    if not h:
        res["classe"] = "bloqueado por ambiente"
        res["sucesso_estrito"] = None
    elif oc is None:
        # sem verificação independente do requisito, "suíte verde" não prova nada (auditoria 2026-11-24)
        res["classe"] = "sem_oraculo"
        res["sucesso_estrito"] = None
    else:
        res["sucesso_estrito"] = bool(concluido and houve_diff and ck.get("validou_depois_da_ultima_edicao") and ck.get("validacao_passou")
                                      and (res["oculta"] or {}).get("passou") and res["suite_depois"] == 0
                                      and res["original_intocado"] and not res["violacao_proibicao"])
        res["classe"] = "sucesso" if res["sucesso_estrito"] else ("concluido_falso" if concluido else "nao_concluido")
    res["causa"] = ck.get("causa")
    os.makedirs(a.saida, exist_ok=True)
    json.dump(res, open(os.path.join(a.saida, rotulo + ".json"), "w"), ensure_ascii=False, indent=1)
    print("%s: %s em %.0f s — causa %s; tentativas %d (válidas %d, cortadas %d); oculta %s" % (
        rotulo, res["classe"], res["duracao_s"], res["causa"], res["tentativas"], res["tentativas_validas"], res["tentativas_truncadas"],
        None if res["oculta"] is None else res["oculta"]["passou"]))
    shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
