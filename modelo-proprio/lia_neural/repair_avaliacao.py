"""V2: avaliação comparativa no holdout. Todo código executado aqui é derivado do gerador sintético por troca atômica de token
(não é código livre de modelo nem de usuário), por isso roda em-processo com limite de tempo (dados._ex)."""
import random, time

import torch

from . import repair_dados as RD, repair_modelo as M, repair_treino as T

K_LACO = 5


@torch.no_grad()
def pontuar(modelo, ex):
    c = M.codificar(ex["pedido"], ex["codigo"], ex["candidatos"], modelo.config["contexto"], getattr(modelo, "bpe", None))
    if c is None:
        return None
    ids, lens, pos, toks, mask, _ = T.lote([(c, 0)], modelo.config.get("pad", M.PAD))
    modelo.eval()
    return modelo(ids, lens, pos, toks, mask)[0].tolist()


def ordem_modelo(modelo, ex):
    s = pontuar(modelo, ex)
    return [] if s is None else sorted(range(len(s)), key=lambda i: -s[i])


def ordem_fonte(ex):
    """B0/B1: candidatas na ordem do arquivo, só dentro da função citada (a regra não conhece nenhuma família)."""
    sp = RD.funcao_span(ex["codigo"], ex["funcao"])
    return [i for i, c in enumerate(ex["candidatos"]) if sp and sp[0] <= c["c0"] < sp[1]]


def _saidas(codigo, ex, entradas):
    return [RD._ex(codigo, ex["funcao"], e) for e in entradas]


def julgar(ex, i, esperado):
    """aplica a candidata i; roda a suíte visível e o oráculo oculto. Só o runtime conclui (visível passa)."""
    novo = RD.aplicar(ex["codigo"], ex["candidatos"][i])
    nv = len(ex["visivel"])
    ob = _saidas(novo, ex, ex["visivel"] + ex["oculto"])
    vis = ob[:nv] == esperado[:nv]
    ocu = vis and ob[nv:] == esperado[nv:]
    return {"visivel_ok": vis, "oculto_ok": ocu, "concluido": vis, "falso_concluido": vis and not ocu}


def avaliar_condicoes(exemplos, modelo_treinado, semente_aleatorio=12345, log=print):
    torch.manual_seed(semente_aleatorio)
    aleatorio = M.Localizador(modelo_treinado.config)
    aleatorio.bpe = getattr(modelo_treinado, "bpe", None)
    ordens = {"aleatorio": lambda e: ordem_modelo(aleatorio, e), "treinado": lambda e: ordem_modelo(modelo_treinado, e), "fonte_b0": ordem_fonte}
    rel = {}
    prep = []
    for e in exemplos:
        esp = _saidas(e["correto"], e, e["visivel"] + e["oculto"])
        prep.append((e, esp, {k: f(e) for k, f in ordens.items()}))
    for nome in ordens:
        t0 = time.time(); rs = []
        for e, esp, od in prep:
            o = od[nome]
            r1 = {"id": e["id"], "familia": e["familia"], "top1_exato": bool(o) and o[0] == e["alvo"], "recall3": e["alvo"] in o[:3]}
            r1.update(julgar(e, o[0], esp) if o else {"visivel_ok": False, "oculto_ok": False, "concluido": False, "falso_concluido": False})
            # laço do runtime: até K candidatas, conclui na primeira que passa a suíte visível
            lac = {"concluido": False, "oculto_ok": False, "falso_concluido": False, "tentativas": 0}
            for i in o[:K_LACO]:
                j = julgar(e, i, esp); lac["tentativas"] += 1
                if j["concluido"]:
                    lac.update(concluido=True, oculto_ok=j["oculto_ok"], falso_concluido=j["falso_concluido"]); break
            rs.append((r1, lac))
        n = max(1, len(rs)); pct = lambda xs: round(100.0 * sum(xs) / n, 2)  # noqa: E731
        rel[nome] = {"top1": {"n": len(rs), "top1_exato_pct": pct([a["top1_exato"] for a, _ in rs]), "recall3_pct": pct([a["recall3"] for a, _ in rs]),
                              "testes_ocultos_aprovados_pct": pct([a["oculto_ok"] for a, _ in rs]), "concluidos": sum(a["concluido"] for a, _ in rs),
                              "falsos_concluido": sum(a["falso_concluido"] for a, _ in rs)},
                     "laco_k%d" % K_LACO: {"testes_ocultos_aprovados_pct": pct([b["oculto_ok"] for _, b in rs]), "concluidos": sum(b["concluido"] for _, b in rs),
                                          "falsos_concluido": sum(b["falso_concluido"] for _, b in rs),
                                          "tentativas_medias": round(sum(b["tentativas"] for _, b in rs) / n, 2)},
                     "segundos": round(time.time() - t0, 1), "resultados": [a for a, _ in rs]}
        log("%-9s top1: ocultos=%.1f%% exato=%.1f%% rec3=%.1f%% falsos=%d | laço: ocultos=%.1f%% falsos=%d tent=%.2f" % (
            nome, rel[nome]["top1"]["testes_ocultos_aprovados_pct"], rel[nome]["top1"]["top1_exato_pct"], rel[nome]["top1"]["recall3_pct"],
            rel[nome]["top1"]["falsos_concluido"], rel[nome]["laco_k%d" % K_LACO]["testes_ocultos_aprovados_pct"],
            rel[nome]["laco_k%d" % K_LACO]["falsos_concluido"], rel[nome]["laco_k%d" % K_LACO]["tentativas_medias"]))
    return rel


def bootstrap_dif(a, b, n=2000, semente=7):
    """IC 95 % da diferença de médias (pontos percentuais) entre duas listas pareadas de 0/1."""
    r = random.Random(semente); m = len(a); ds = []
    for _ in range(n):
        idx = [r.randrange(m) for _ in range(m)]
        ds.append(100.0 * sum(a[i] - b[i] for i in idx) / m)
    ds.sort()
    return [round(ds[int(0.025 * n)], 2), round(ds[int(0.975 * n)], 2)]


def veredito(rel):
    t, a, b = (rel[k]["top1"] for k in ("treinado", "aleatorio", "fonte_b0"))
    ok = lambda k: [int(r["oculto_ok"]) for r in rel[k]["resultados"]]  # noqa: E731
    v = {"ocultos_treinado": t["testes_ocultos_aprovados_pct"], "ocultos_aleatorio": a["testes_ocultos_aprovados_pct"], "ocultos_b0": b["testes_ocultos_aprovados_pct"],
         "treinado_menos_aleatorio_pp": round(t["testes_ocultos_aprovados_pct"] - a["testes_ocultos_aprovados_pct"], 2),
         "treinado_menos_b0_pp": round(t["testes_ocultos_aprovados_pct"] - b["testes_ocultos_aprovados_pct"], 2),
         "ic95_vs_aleatorio_pp": bootstrap_dif(ok("treinado"), ok("aleatorio")), "ic95_vs_b0_pp": bootstrap_dif(ok("treinado"), ok("fonte_b0")),
         "falsos_concluido_treinado": t["falsos_concluido"]}
    v["capacidade_declaravel_na_tarefa_estreita"] = (v["treinado_menos_aleatorio_pp"] >= 10 and v["treinado_menos_b0_pp"] >= 10 and t["falsos_concluido"] == 0)
    return v
