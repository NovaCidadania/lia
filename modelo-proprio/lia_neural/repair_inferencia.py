"""V2: inferência offline e adaptador para o prompt de coding da Lia. A rede só ORDENA trocas atômicas; escolher o escopo (função citada
existente, um único arquivo) é REGRA determinística daqui, rotulada como tal. Nada é escrito: aplicar/diff/aprovação/teste é do runtime."""
import hashlib, re

from . import repair_checkpoint as C, repair_dados as RD, repair_modelo as M
from .inferencia import extrair_do_prompt_lia, SEM_ESCOPO
from .repair_avaliacao import pontuar


def funcao_citada(pedido, codigo):
    for n in re.findall(r"^def (\w+)\(", codigo, re.M):
        if re.search(r"\b%s\b" % re.escape(n), pedido):
            return n
    return None


def propor(modelo, pedido, codigo, funcao, k=1):
    """-> lista das k melhores edições [{procurar, substituir, antigo, novo, escore}] dentro da função; [] se nada cabe."""
    cands = RD.candidatos(codigo)
    sp = RD.funcao_span(codigo, funcao)
    if not cands or not sp:
        return []
    ex = {"pedido": pedido, "codigo": codigo, "candidatos": cands}
    s = pontuar(modelo, ex)
    if s is None:
        return []
    ordem = sorted((i for i, c in enumerate(cands) if sp[0] <= c["c0"] < sp[1]), key=lambda i: -s[i])
    return [dict(procurar=cands[i]["procurar"], substituir=cands[i]["substituir"], antigo=cands[i]["antigo"], novo=cands[i]["novo"], escore=round(s[i], 4))
            for i in ordem[:k]]


def propor_para_lia(checkpoint, prompt):
    modelo, d = C.carregar(checkpoint)
    ident = {"backend": "lia_proprio", "modelo": "localizador-v2", "checkpoint_sha256": hashlib.sha256(open(checkpoint, "rb").read()).hexdigest(),
             "parametros": modelo.n_parametros(), "passo": d["passo"]}
    try:
        pedido, caminho, codigo = extrair_do_prompt_lia(prompt)
    except ValueError as e:
        return {"edicoes": [], "explicacao": "%s: %s" % (SEM_ESCOPO, e), "modelo": ident, "motivo": "fora_do_escopo"}
    f = funcao_citada(pedido, codigo)
    if f is None:                                       # regra determinística de escopo (não é capacidade do modelo)
        return {"edicoes": [], "explicacao": "não achei no arquivo a função citada no pedido; diga qual função corrigir", "modelo": ident, "motivo": "esclarecer"}
    p = propor(modelo, pedido, codigo, f, 1)
    if not p:
        return {"edicoes": [], "explicacao": "nenhuma troca atômica candidata em %s" % f, "modelo": ident, "motivo": "sem_candidata"}
    return {"edicoes": [{"acao": "editar", "caminho": caminho, "procurar": p[0]["procurar"], "substituir": p[0]["substituir"]}],
            "explicacao": "troca de `%s` por `%s` em %s, proposta pelo localizador próprio (%s)" % (p[0]["antigo"], p[0]["novo"], f, ident["checkpoint_sha256"][:12]),
            "modelo": ident, "motivo": "editar"}
