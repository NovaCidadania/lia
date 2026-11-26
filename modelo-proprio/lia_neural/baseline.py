"""Baselines determinísticos SEM aprendizado, para comparação.

B0 (o do critério de conclusão): na função citada no pedido, inverte o primeiro operador da lista fixa que aparecer (de cima
para baixo no corpo). Não conhece nenhuma família e não roda nada. Se o arquivo não tem a função citada, pede contexto.

B1 (referência mais forte, NÃO usada no critério): enumera todas as inversões de um operador e fica com a primeira que passa a
suíte visível — usa informação (testes) que o modelo não recebe."""
import re

from . import sandbox

FLIPS = [("<=", ">="), (">=", "<="), ("==", "!="), ("!=", "=="), ("<", ">"), (">", "<"), ("+=", "-="), ("-=", "+="), (" + ", " - "), (" - ", " + "), (" * ", " + ")]


def _funcao_citada(pedido, codigo):
    nomes = re.findall(r"^def (\w+)\(", codigo, re.M)
    for n in nomes:
        if re.search(r"\b%s\b" % re.escape(n), pedido):
            return n
    return None


def _candidatas(codigo, nome):
    """(procurar, substituir) de cada inversão possível no corpo da função, na ordem de FLIPS e de aparição"""
    linhas = codigo.split("\n")
    ini = next(i for i, l in enumerate(linhas) if l.startswith("def %s(" % nome))
    fim = ini + 1
    while fim < len(linhas) and (linhas[fim].startswith(" ") or not linhas[fim].strip()):
        fim += 1
    out = []
    for velho, novo in FLIPS:
        for i in range(ini + 1, fim):
            if velho in linhas[i]:
                out.append((linhas[i], linhas[i].replace(velho, novo, 1)))
                break
    return out


def b0(pedido, codigo):
    nome = _funcao_citada(pedido, codigo)
    if not nome:
        return {"tipo": "esclarecer", "texto": "Não encontrei a função citada."}
    c = _candidatas(codigo, nome)
    if not c:
        return {"tipo": "esclarecer", "texto": "Não achei o que alterar."}
    p, s = c[0]
    return {"tipo": "editar", "procurar": p, "substituir": s}


def b1(pedido, codigo, visivel, esperado_visivel):
    nome = _funcao_citada(pedido, codigo)
    if not nome:
        return {"tipo": "esclarecer", "texto": "Não encontrei a função citada."}
    for p, s in _candidatas(codigo, nome):
        if codigo.count(p) != 1:
            continue
        if sandbox.rodar_casos(codigo.replace(p, s, 1), nome, visivel) == esperado_visivel:
            return {"tipo": "editar", "procurar": p, "substituir": s}
    return {"tipo": "esclarecer", "texto": "Nenhuma inversão passou a suíte visível."}
