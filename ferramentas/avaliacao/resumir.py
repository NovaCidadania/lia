#!/usr/bin/env python3
"""Agrega execuções de avaliar.py por (conjunto, condição): sucesso estrito,
"concluído" falso, bloqueadas por ambiente, propostas válidas, truncamento,
tentativas, tempo, tokens, violações e causas. Uso: resumir.py DIR [--md]"""
import json, os, statistics, sys
from collections import Counter, defaultdict


def conjunto(r):
    t = r["tarefa"]
    return t.get("conjunto") or "congelado"


def main():
    d = sys.argv[1]
    md = "--md" in sys.argv
    runs = [json.load(open(os.path.join(d, f))) for f in sorted(os.listdir(d)) if f.endswith(".json")]
    g = defaultdict(list)
    for r in runs:
        cond = r["condicao"] + (" T=%s" % r["parametros"].get("LIA_MODELO_TEMPERATURA") if r["parametros"].get("LIA_MODELO_TEMPERATURA") else "") \
               + (" [loop simples]" if r["parametros"].get("LIA_SESSIOND_ARVORE_PORTAO") == "0" else "")
        g[(conjunto(r), cond)].append(r)
    linhas = []
    cab = ["conjunto", "condição", "execuções", "sucesso estrito", "concluído falso", "sem oráculo/bloqueadas", "propostas válidas/total",
           "truncadas", "tentativas méd.", "tempo mediano (s)", "tokens ger. méd.", "violações", "causas"]
    for (cj, cond), rs in sorted(g.items()):
        aval = [r for r in rs if r.get("sucesso_estrito") is not None]
        suc = sum(1 for r in aval if r["sucesso_estrito"])
        falso = sum(1 for r in rs if r.get("classe") == "concluido_falso")
        bloq = sum(1 for r in rs if r.get("sucesso_estrito") is None)  # inclui classe sem_oraculo
        tv = sum(r.get("tentativas_validas", 0) for r in rs)
        tt = sum(r.get("tentativas", 0) for r in rs)
        tr = sum(r.get("tentativas_truncadas", 0) for r in rs)
        causas = Counter(r.get("causa") or "?" for r in rs)
        linhas.append([cj, cond, str(len(rs)), "%d/%d" % (suc, len(aval)), str(falso), str(bloq), "%d/%d" % (tv, tt), str(tr),
                       "%.1f" % (tt / len(rs)), "%.0f" % statistics.median(r["duracao_s"] for r in rs),
                       "%.0f" % (sum(r.get("tokens_gerados", 0) for r in rs) / len(rs)), str(sum(1 for r in rs if r.get("violacao_proibicao"))),
                       ", ".join("%s %d" % kv for kv in causas.most_common())])
    if md:
        print("| " + " | ".join(cab) + " |")
        print("|" + "---|" * len(cab))
        for l in linhas:
            print("| " + " | ".join(l) + " |")
    else:
        for l in linhas:
            print(dict(zip(cab, l)))
    # por tarefa
    print()
    if md:
        print("| execução | classe | causa | tentativas (válidas/cortadas) | oculta | suíte depois | tempo (s) | tokens ger. |")
        print("|---|---|---|---|---|---|---|---|")
    for r in sorted(runs, key=lambda r: (conjunto(r), r["tarefa"]["id"], r["condicao"], r["rep"])):
        oc = r.get("oculta")
        cel = [os.path.basename(r["tarefa"]["id"]) + " · " + r["condicao"] + " · r%d" % r["rep"], r.get("classe"), str(r.get("causa")),
               "%d (%d/%d)" % (r.get("tentativas", 0), r.get("tentativas_validas", 0), r.get("tentativas_truncadas", 0)),
               "—" if oc is None else ("passou" if oc["passou"] else "falhou"), str(r.get("suite_depois")), "%.0f" % r["duracao_s"],
               str(r.get("tokens_gerados", 0))]
        print(("| " + " | ".join(cel) + " |") if md else cel)


if __name__ == "__main__":
    main()
