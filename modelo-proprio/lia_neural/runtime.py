"""Runtime sintético de avaliação: aplica a edição proposta, roda a suíte VISÍVEL e o oráculo OCULTO em sandbox.
Só ele "conclui": o modelo não tem como declarar tarefa concluída. Falso "concluído" = suíte visível passou e o oráculo reprovou."""
from . import sandbox


def _igual(a, b):
    return len(a) == len(b) and all(x == y for x, y in zip(a, b))


def julgar(ex, proposta):
    r = {"id": ex["id"], "familia": ex["familia"], "tipo_esperado": ex["tipo"], "formato_valido": proposta is not None,
         "tipo_proposto": proposta["tipo"] if proposta else None, "aplica": False, "visivel_ok": False, "oculto_ok": False,
         "concluido": False, "falso_concluido": False, "acao_correta": False, "edicao_indevida": False}
    if proposta is None:
        return r
    if ex["tipo"] != "editar":
        r["acao_correta"] = proposta["tipo"] == ex["tipo"]
        r["edicao_indevida"] = proposta["tipo"] == "editar"      # sem suíte para validar, o runtime não conclui nada
        return r
    if proposta["tipo"] != "editar":
        return r                                               # pediu contexto/recusou onde havia o que corrigir: não resolve
    proc = proposta["procurar"]
    if ex["codigo"].count(proc) != 1:
        return r                                               # trecho ausente ou ambíguo: o verificador recusa a edição
    r["aplica"] = True
    novo = ex["codigo"].replace(proc, proposta["substituir"], 1)
    ent = ex["visivel"] + ex["oculto"]
    obtido = sandbox.rodar_casos(novo, ex["funcao"], ent)
    esperado = sandbox.rodar_casos(ex["correto"], ex["funcao"], ent)
    nv = len(ex["visivel"])
    r["visivel_ok"] = _igual(obtido[:nv], esperado[:nv])
    r["oculto_ok"] = r["visivel_ok"] and _igual(obtido[nv:], esperado[nv:])
    r["concluido"] = r["visivel_ok"]                           # critério do runtime: suíte visível passou depois da edição
    r["falso_concluido"] = r["concluido"] and not r["oculto_ok"]
    r["acao_correta"] = r["oculto_ok"]
    return r


def agregar(rs):
    ed = [r for r in rs if r["tipo_esperado"] == "editar"]
    ou = [r for r in rs if r["tipo_esperado"] != "editar"]
    pct = lambda xs, k: round(100.0 * sum(1 for r in xs if r[k]) / max(1, len(xs)), 2)  # noqa: E731
    return {"n": len(rs), "n_editar": len(ed), "n_esclarecer_ou_recusar": len(ou),
            "formato_valido_pct": pct(rs, "formato_valido"),
            "aplica_pct": pct(ed, "aplica"), "visivel_ok_pct": pct(ed, "visivel_ok"),
            "testes_ocultos_aprovados_pct": pct(ed, "oculto_ok"),
            "concluidos": sum(1 for r in rs if r["concluido"]), "falsos_concluido": sum(1 for r in rs if r["falso_concluido"]),
            "acao_correta_esclarecer_recusar_pct": pct(ou, "acao_correta"), "edicoes_indevidas": sum(1 for r in ou if r["edicao_indevida"])}
