"""Famílias COMPOSICIONAIS (V2, só treino): laço sobre lista = agregação × filtro × transformação, com a constante K sorteada.
A descrição em português é composta das mesmas partes que geram o código, então cada token do código tem correspondente no pedido.
Isso dá milhares de esqueletos distintos (em vez de ~45) para a rede aprender a relação palavra→operador/constante, sem copiar testes."""
import random

AGG = {"soma": ("0", "+="), "conta": ("0", "+="), "produto": ("1", "*=")}
FILTROS = [  # (condição, texto)
    ("v > {K}", "maiores que {K}"), ("v < {K}", "menores que {K}"), ("v >= {K}", "maiores ou iguais a {K}"), ("v <= {K}", "menores ou iguais a {K}"),
    ("v % 2 == 0", "pares"), ("v % 2 == 1", "ímpares"), ("v != {K}", "diferentes de {K}"), (None, "da lista")]
TRANS = [("v", ""), ("v * v", " ao quadrado"), ("v * 2", " dobrados"), ("v + 1", ", cada um mais 1")]


def compor(r):
    agg = r.choice(list(AGG)); cond, ft = r.choice(FILTROS); K = r.randint(0, 6)
    ini, op = AGG[agg]
    tr, tt = r.choice(TRANS) if agg != "conta" else ("1", "")
    if agg == "produto" and tr == "v":
        tr = r.choice(("v", "v + 1"))
        tt = "" if tr == "v" else ", cada um mais 1"
    nome = {"soma": "soma", "conta": "conta quantos são", "produto": "multiplica"}[agg]
    desc = ("conta os elementos %s" % ft.format(K=K)) if agg == "conta" else "%s os elementos %s%s" % (nome.replace(" quantos são", ""), ft.format(K=K), tt)
    corpo = "total %s %s" % (op, tr)
    if cond:
        src = "def {f}(valores):\n    total = %s\n    for v in valores:\n        if %s:\n            %s\n    return total\n" % (ini, cond.format(K=K), corpo)
    else:
        src = "def {f}(valores):\n    total = %s\n    for v in valores:\n        %s\n    return total\n" % (ini, corpo)
    return dict(id="comp_%s_%s_%s_%d" % (agg, (cond or "tudo").replace(" ", "").replace("%", "m"), tr.replace(" ", ""), K), d=desc, entrada="lst", src=src)


def familias_composicionais(n, semente=20261126):
    r = random.Random("comp|%s" % semente); vistos, out = set(), []
    while len(out) < n:
        f = compor(r)
        if f["id"] not in vistos:
            vistos.add(f["id"]); out.append(f)
    return out
