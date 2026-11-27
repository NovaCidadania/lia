"""Famílias de EXPRESSÃO (V2, só treino): função de a, b com ramos e condições sorteados; a descrição em português repete a regra com as mesmas
constantes e operadores. Ensina a conferir 'o que o pedido diz' contra 'o que o código faz' em milhares de esqueletos distintos."""
import random

CMP = {"<": "menor que", "<=": "menor ou igual a", ">": "maior que", ">=": "maior ou igual a", "==": "igual a", "!=": "diferente de"}
OPS = {"+": "mais", "-": "menos", "*": "vezes"}


def _expr(r, prof=2):
    if prof == 0 or r.random() < 0.3:
        return r.choice(("a", "b", str(r.randint(1, 9))))
    e1, e2 = _expr(r, prof - 1), _expr(r, prof - 1)
    return "%s %s %s" % (e1, r.choice(list(OPS)), e2)


def _txt(e):
    for o, t in OPS.items():
        e = e.replace(" %s " % o, " %s " % t)
    return e


def _cond(r):
    v, op, k = r.choice("ab"), r.choice(list(CMP)), r.randint(0, 9)
    return "%s %s %d" % (v, op, k), "%s %s %d" % (v, CMP[op], k)


def expressao(r):
    c, ct = _cond(r)
    if r.random() < 0.35:
        c2, c2t = _cond(r); j = r.choice(("and", "or"))
        c, ct = "%s %s %s" % (c, j, c2), "%s %s %s" % (ct, "e" if j == "and" else "ou", c2t)
    e1, e2 = _expr(r), _expr(r)
    if e1 == e2:
        e2 = "%s + 1" % e2
    src = "def {f}(a, b):\n    if %s:\n        return %s\n    return %s\n" % (c, e1, e2)
    return dict(id="expr_%s|%s|%s" % (c, e1, e2), d="devolve %s se %s, senão %s" % (_txt(e1), ct, _txt(e2)), entrada="ab", src=src)


def familias_expressao(n, semente=20261126):
    r = random.Random("expr|%s" % semente); vistos, out = set(), []
    while len(out) < n:
        f = expressao(r)
        if f["id"] not in vistos:
            vistos.add(f["id"]); out.append(f)
    return out
