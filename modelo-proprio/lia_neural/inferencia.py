"""Inferência local offline + adaptador para o prompt do coding da Lia (que vira o formato nativo do modelo)."""
import json, re

from . import tokenizer as tk
from .checkpoint import carregar

SEM_ESCOPO = "fora do escopo do modelo"


def propor(modelo, pedido, codigo, max_novos=220):
    """-> dict(proposta=interpretar(...)|None, tokens, ids). O formato é validado de forma estrita."""
    prompt = tk.montar_prompt(pedido, codigo)
    if len(prompt) >= modelo.config["contexto"] - 8:
        return {"proposta": None, "tokens": 0, "ids": [], "motivo": "contexto_maior_que_o_modelo"}
    ids = modelo.gerar(prompt, max_novos)
    return {"proposta": tk.interpretar(ids), "tokens": len(ids), "ids": ids, "motivo": "ok"}


def extrair_do_prompt_lia(prompt):
    """pedido e (único) arquivo do prompt de coding da Lia (sessiond/coding.c: coding_prompt_edicoes).
    -> (pedido, caminho, codigo) ou levanta ValueError com o motivo (o modelo só entende UM arquivo curto)."""
    m = re.search(r"----- pedido -----\n(.*?)\n----- critérios de aceite -----", prompt, re.S)
    if not m:
        raise ValueError("o prompt não é um pedido de edição de código da Lia")
    pedido = m.group(1).strip()
    arqs = re.findall(r"^=== (.+?)(?: \(.*\))? ===\n(.*?)(?=^=== |\n----- |\Z)", prompt.split("----- arquivos lidos -----", 1)[-1], re.S | re.M)
    if len(arqs) != 1:
        raise ValueError("o modelo trata exatamente 1 arquivo (recebeu %d)" % len(arqs))
    caminho, codigo = arqs[0]
    return pedido, caminho.strip(), codigo.rstrip("\n") + "\n"


def propor_para_lia(checkpoint, prompt):
    """Texto JSON no contrato de edições da Lia: {"edicoes":[{"acao":"editar","caminho","procurar","substituir"}],"explicacao"}.
    Falha com CheckpointInvalido se o checkpoint faltar/for inválido — nunca recai em outro modelo.
    O modelo só PROPÕE; aplicar, mostrar o diff, pedir aprovação, escrever e testar é do runtime da Lia."""
    modelo, d = carregar(checkpoint)
    ident = {"backend": "lia_proprio", "checkpoint_sha256": __import__("hashlib").sha256(open(checkpoint, "rb").read()).hexdigest(),
             "parametros": modelo.n_parametros(), "passo": d["passo"]}
    try:
        pedido, caminho, codigo = extrair_do_prompt_lia(prompt)
    except ValueError as e:
        return {"edicoes": [], "explicacao": "%s: %s" % (SEM_ESCOPO, e), "modelo": ident, "motivo": "fora_do_escopo"}
    r = propor(modelo, pedido, codigo)
    p = r["proposta"]
    if p is None:
        return {"edicoes": [], "explicacao": "saída do modelo em formato inválido (%s)" % r["motivo"], "modelo": ident, "motivo": "formato_invalido"}
    if p["tipo"] != "editar":
        return {"edicoes": [], "explicacao": p["texto"], "modelo": ident, "motivo": p["tipo"]}
    return {"edicoes": [{"acao": "editar", "caminho": caminho, "procurar": p["procurar"], "substituir": p["substituir"]}],
            "explicacao": "edição proposta pelo modelo próprio (%s)" % ident["checkpoint_sha256"][:12], "modelo": ident, "motivo": "editar"}
