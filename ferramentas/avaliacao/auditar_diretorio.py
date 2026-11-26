#!/usr/bin/env python3
"""Auditoria de rastreabilidade de um diretório de resultados (somente leitura sobre os resultados).

Lê cada <rotulo>.json, confere metadados (versão/sha do avaliador, sha do lia-sessiond, tarefas, condição, atribuição),
cruza com manifesto.json e escreve AUDITORIA.json ao lado (arquivo NOVO; nenhum resultado histórico é alterado).
Um diretório com versões de avaliador ou de binário diferentes NÃO é uma matriz: é marcado 'misto' e 'comparavel: false'.

  auditar_diretorio.py <dir> [--sem-escrever]"""
import hashlib, json, os, sys

CAMPOS = ("avaliador_versao", "avaliador_sha256", "lia_sessiond_sha256", "classe", "atribuicao")


def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


def auditar(d):
    arqs = sorted(f for f in os.listdir(d) if f.endswith(".json") and f not in ("manifesto.json", "AUDITORIA.json"))
    itens, avisos = [], []
    man = None
    if os.path.exists(os.path.join(d, "manifesto.json")):
        man = json.load(open(os.path.join(d, "manifesto.json")))
    for f in arqs:
        p = os.path.join(d, f)
        try:
            r = json.load(open(p))
        except (OSError, ValueError) as e:
            itens.append({"arquivo": f, "erro": str(e)})
            avisos.append("%s ilegível" % f)
            continue
        it = {"arquivo": f, "sha256": sha(p), "rotulo": f[:-5], "condicao": r.get("condicao") or r.get("modelo"), "tarefa": (r.get("tarefa") or {}).get("id") if isinstance(r.get("tarefa"), dict) else r.get("tarefa")}
        for k in CAMPOS:
            it[k] = r.get(k)
        for k in CAMPOS[:3]:
            if not it[k]:
                avisos.append("%s sem %s" % (f, k))
        itens.append(it)
    chaves = lambda k: sorted({str(i.get(k)) for i in itens if i.get(k)})  # noqa: E731
    versoes, shas_av, shas_bin = chaves("avaliador_versao"), chaves("avaliador_sha256"), chaves("lia_sessiond_sha256")
    misto = len(shas_av) > 1 or len(shas_bin) > 1 or len(versoes) > 1
    if misto:
        avisos.append("resultados de mais de uma versão/binário do avaliador no mesmo diretório: não é uma única matriz")
    no_manifesto = {v["rotulo"] for v in (man or {}).get("validas", [])} | {v.get("rotulo") for v in (man or {}).get("reaproveitadas", []) if isinstance(v, dict)}
    fora = [i["rotulo"] for i in itens if i["rotulo"] not in no_manifesto]
    if man is None:
        avisos.append("sem manifesto.json")
    elif fora:
        avisos.append("JSONs não contabilizados no manifesto: %s" % ", ".join(fora))
    return {"formato": "lia-auditoria-diretorio/1", "diretorio": os.path.abspath(d), "resultados": len(itens), "itens": itens,
            "avaliador_versoes": versoes, "avaliador_sha256": shas_av, "lia_sessiond_sha256": shas_bin,
            "manifesto_esperadas": (man or {}).get("esperadas"), "manifesto_validas": len((man or {}).get("validas", [])),
            "misto": misto, "cobertura_confere_com_manifesto": man is not None and not fora,
            "comparavel": bool(man and not misto and not fora and man.get("comparavel") and len({i.get("condicao") for i in itens}) > 1),   # um smoke de UMA condição nunca é uma comparação
            "avisos": avisos}


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    a = auditar(sys.argv[1])
    if "--sem-escrever" not in sys.argv:
        with open(os.path.join(sys.argv[1], "AUDITORIA.json"), "w") as f:
            json.dump(a, f, ensure_ascii=False, indent=1)
    print("%s: %d resultados, misto=%s, confere_manifesto=%s, comparavel=%s" % (sys.argv[1], a["resultados"], a["misto"], a["cobertura_confere_com_manifesto"], a["comparavel"]))
    for v in a["avisos"]:
        print("  AVISO:", v)
