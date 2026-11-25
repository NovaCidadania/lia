#!/usr/bin/env python3
"""Runner de matriz de avaliação, fail-fast e retomável.

  matriz.py --tarefas ARQ.json --saida DIR --condicao COND[=HASH_DO_MODELO] ... --sementes 1 2 3 [--temperatura 0.4] [--ids ...]

Garantias (auditoria 2026-11-25):
  - cada combinação (tarefa x condição x semente) roda no avaliador; código de saída E JSON são conferidos;
  - só pula uma combinação cujo resultado existente confere TODOS os metadados da execução esperada
    (tarefa, condição, semente, hash do modelo, parâmetros, hash do arquivo de tarefas, hash do binário,
    versão e hash do avaliador, estado terminal) — resultado de outra versão é refeito, nunca reaproveitado;
  - falha do harness (exit != 0, JSON ausente/inválido, .harness-erro.json) NÃO é resultado do modelo;
  - 'FIM' só sai se TODAS as combinações esperadas têm resultado terminal válido; senão 'INCOMPLETO';
  - manifesto.json (atômico) com esperadas, válidas, faltantes, falhas de harness e totais por condição;
  - SIGTERM/SIGINT: encerra o avaliador e toda a sua descendência (inclusive o daemon em sessão própria).
Exit: 0 completo; 1 incompleto; 130 interrompido."""
import argparse, hashlib, json, os, signal, subprocess, sys, time

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
import protocolo  # noqa: E402

CLASSES = ("sucesso", "concluido_falso", "nao_concluido", "sem_oraculo", "bloqueado por ambiente")
_filho = [None]


def rotulo(tid, cond, rep, temp):
    return "%s__%s__r%d%s" % (tid, cond.replace(":", "_").replace("/", "_"), rep, ("_T" + temp) if temp else "")


def gravar_atomico(caminho, doc):
    tmp = caminho + ".tmp.%d" % os.getpid()
    with open(tmp, "w") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, caminho)


def validar(res, esp):
    """lista de motivos pelos quais 'res' NÃO é o resultado terminal válido da execução 'esp' (vazia = válido)"""
    erros = []
    if not isinstance(res, dict):
        return ["resultado não é um objeto JSON"]
    for k, v in (("formato", esp["formato"]), ("avaliador_versao", esp["avaliador_versao"]), ("avaliador_sha256", esp["avaliador_sha256"]),
                 ("lia_sessiond_sha256", esp["lia_sessiond_sha256"]), ("tarefas_sha256", esp["tarefas_sha256"]), ("condicao", esp["condicao"]),
                 ("rep", esp["rep"]), ("modelo_hash", esp["modelo_hash"])):
        if res.get(k) != v:
            erros.append("%s difere (esperado %r, no resultado %r)" % (k, v, res.get(k)))
    if (res.get("tarefa") or {}).get("id") != esp["id"]:
        erros.append("tarefa.id difere")
    par = res.get("parametros") or {}
    if esp["temperatura"] and par.get("LIA_MODELO_TEMPERATURA") != esp["temperatura"]:
        erros.append("temperatura difere")
    if esp["condicao"] != "sem-modelo" and str(par.get("LIA_MODELO_SEMENTE", "")) != str(esp["rep"]):
        erros.append("semente difere")
    if res.get("estado_terminal") != "ok":
        erros.append("estado_terminal %r (esperado 'ok')" % res.get("estado_terminal"))
    if res.get("classe") not in CLASSES:
        erros.append("classe %r não é terminal" % res.get("classe"))
    if not res.get("commit_base"):
        erros.append("commit_base ausente")
    if "sucesso_estrito" not in res or "duracao_s" not in res:
        erros.append("campos obrigatórios ausentes (sucesso_estrito/duracao_s)")
    elif (res.get("classe") == "sucesso") != (res.get("sucesso_estrito") is True):
        erros.append("classe 'sucesso' inconsistente com sucesso_estrito")
    return erros


def matar_filho_e_sair(sig, _f):
    p = _filho[0]
    if p is not None:
        protocolo.encerrar_arvore(p, espera=10.0)
    raise KeyboardInterrupt(sig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tarefas", required=True)
    ap.add_argument("--saida", required=True)
    ap.add_argument("--condicao", action="append", required=True, help="COND ou COND=HASH_DO_MODELO (repetível)")
    ap.add_argument("--sementes", nargs="+", type=int, default=[1, 2, 3])
    ap.add_argument("--temperatura", default="0.4")
    ap.add_argument("--ids", nargs="*", default=None)
    ap.add_argument("--avaliador", default=os.path.join(AQUI, "avaliar.py"))
    ap.add_argument("--limite-min", type=int, default=40)
    ap.add_argument("--sem-progresso-s", type=int, default=0)
    ap.add_argument("--folga-s", type=int, default=600, help="além do limite do avaliador, antes de o runner matá-lo")
    a = ap.parse_args()
    os.makedirs(a.saida, exist_ok=True)
    doc = json.load(open(a.tarefas))
    ids = a.ids or [t["id"] for t in doc["tarefas"]]
    tarefas_sha = hashlib.sha256(open(a.tarefas, "rb").read()).hexdigest()
    py = [sys.executable, "-I"]
    ver = json.loads(subprocess.run(py + [a.avaliador, "--versao"], capture_output=True, text=True, check=True).stdout)
    conds = []
    for c in a.condicao:
        nome, _, h = c.partition("=")
        conds.append((nome, h))
    esperadas = [{"id": t, "condicao": c, "modelo_hash": h, "rep": s, "temperatura": a.temperatura, "tarefas_sha256": tarefas_sha, **ver}
                 for c, h in conds for s in a.sementes for t in ids]
    estado = {"formato": "lia-matriz-manifesto/1", "tarefas_arquivo": os.path.basename(a.tarefas), "tarefas_sha256": tarefas_sha,
              "avaliador_versao": ver["avaliador_versao"], "avaliador_sha256": ver["avaliador_sha256"], "lia_sessiond_sha256": ver["lia_sessiond_sha256"],
              "temperatura": a.temperatura, "sementes": a.sementes, "esperadas": len(esperadas), "validas": [], "reaproveitadas": [],
              "falhas_de_harness": [], "faltantes": [], "interrompido": False, "iniciado": time.strftime("%Y-%m-%dT%H:%M:%S")}

    def manifesto():
        feitas = {x["rotulo"] for x in estado["validas"]}
        estado["faltantes"] = [rotulo(e["id"], e["condicao"], e["rep"], e["temperatura"]) for e in esperadas
                               if rotulo(e["id"], e["condicao"], e["rep"], e["temperatura"]) not in feitas]
        por = {}
        for x in estado["validas"]:
            d = por.setdefault(x["condicao"], {c: 0 for c in CLASSES})
            d[x["classe"]] += 1
        estado["por_condicao"] = por
        estado["completo"] = not estado["faltantes"] and not estado["falhas_de_harness"] and not estado["interrompido"]
        gravar_atomico(os.path.join(a.saida, "manifesto.json"), estado)

    for sg in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sg, matar_filho_e_sair)
    try:
        for e in esperadas:
            rot = rotulo(e["id"], e["condicao"], e["rep"], e["temperatura"])
            alvo = os.path.join(a.saida, rot + ".json")
            if os.path.exists(alvo):
                try:
                    res = json.load(open(alvo))
                    motivos = validar(res, e)
                except (OSError, ValueError) as ex:
                    res, motivos = None, ["JSON ilegível: %s" % ex]
                if not motivos:
                    print("pulado (resultado válido, mesmos metadados): " + rot, flush=True)
                    estado["validas"].append({"rotulo": rot, "condicao": e["condicao"], "classe": res["classe"]})
                    estado["reaproveitadas"].append(rot)
                    manifesto()
                    continue
                print("existente NÃO confere (%s) — refeito: %s" % ("; ".join(motivos)[:200], rot), flush=True)
                os.replace(alvo, alvo + ".descartado")
            cmd = py + [a.avaliador, "--tarefas", a.tarefas, "--id", e["id"], "--condicao", e["condicao"], "--rep", str(e["rep"]),
                        "--saida", a.saida, "--modelo-hash", e["modelo_hash"], "--limite-min", str(a.limite_min)]
            if e["condicao"] != "sem-modelo":
                cmd += ["--temperatura", a.temperatura, "--semente", str(e["rep"])]
            elif a.temperatura:
                cmd += ["--temperatura", a.temperatura]
            if a.sem_progresso_s:
                cmd += ["--sem-progresso-s", str(a.sem_progresso_s)]
            t0 = time.time()
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
            _filho[0] = p
            try:
                saida, _ = p.communicate(timeout=a.limite_min * 60 + a.folga_s)
                rc = p.returncode
            except subprocess.TimeoutExpired:
                protocolo.encerrar_arvore(p)
                saida, rc = "(o avaliador excedeu o limite do runner e foi encerrado com toda a sua árvore)", 124
            _filho[0] = None
            print(saida.strip()[-600:], flush=True)
            motivos = []
            if rc != 0:
                motivos.append("avaliador saiu com código %d" % rc)
            res = None
            try:
                res = json.load(open(alvo))
                motivos += validar(res, e)
            except (OSError, ValueError):
                motivos.append("JSON de resultado ausente ou inválido")
            if motivos:
                herr = os.path.join(a.saida, rot + ".harness-erro.json")
                estado["falhas_de_harness"].append({"rotulo": rot, "motivos": motivos, "codigo_saida": rc, "duracao_s": round(time.time() - t0, 1),
                                                    "detalhe": (json.load(open(herr)).get("harness_erro") if os.path.exists(herr) else None)})
                if os.path.exists(alvo):
                    os.replace(alvo, alvo + ".invalido")
                print("FALHA DE HARNESS em %s: %s" % (rot, "; ".join(motivos)), flush=True)
            else:
                estado["validas"].append({"rotulo": rot, "condicao": e["condicao"], "classe": res["classe"]})
            manifesto()
    except KeyboardInterrupt:
        estado["interrompido"] = True
        manifesto()
        print("INTERROMPIDO: %d de %d combinações com resultado válido; nada foi declarado completo" % (len(estado["validas"]), len(esperadas)), flush=True)
        return 130
    manifesto()
    if estado["completo"]:
        print("FIM: %d de %d combinações com resultado terminal válido (manifesto.json)" % (len(estado["validas"]), len(esperadas)), flush=True)
        return 0
    print("INCOMPLETO: %d de %d válidas; %d falhas de harness; %d faltantes (manifesto.json) — sem FIM" % (
        len(estado["validas"]), len(esperadas), len(estado["falhas_de_harness"]), len(estado["faltantes"])), flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
