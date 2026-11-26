"""Tokenizer da Lia: bytes UTF-8 (ids 0-255) + 12 marcadores estruturais (ids 256-267). Sem treino, sem arquivo externo.

Os marcadores NÃO são produzíveis a partir de texto: só entram pela estrutura (montar_prompt / alvo_*), então um pedido do
usuário que contenha "[FIM]" é só bytes e nunca vira marcador (testado).

Formato nativo
  prompt : BOS PEDIDO <bytes do pedido> CODIGO <bytes do arquivo> RESPOSTA
  alvo   : EDITAR PROCURAR <bytes> SUBSTITUIR <bytes> FIM  |  ESCLARECER <bytes> FIM  |  RECUSAR <bytes> FIM
O formato não tem campo "concluído": o modelo não pode declarar uma tarefa concluída."""
import hashlib

NOMES = ["PAD", "BOS", "EOS", "PEDIDO", "CODIGO", "RESPOSTA", "EDITAR", "PROCURAR", "SUBSTITUIR", "ESCLARECER", "RECUSAR", "FIM"]
PAD, BOS, EOS, PEDIDO, CODIGO, RESPOSTA, EDITAR, PROCURAR, SUBSTITUIR, ESCLARECER, RECUSAR, FIM = range(256, 268)
VOCAB = 268
VERSAO = "bytes+12/1"


def bytes_para_ids(b):
    return list(b)


def texto_para_ids(t):
    return list(t.encode("utf-8"))


def ids_para_texto(ids):
    return bytes(i for i in ids if i < 256).decode("utf-8", errors="replace")


def montar_prompt(pedido, codigo):
    return [BOS, PEDIDO] + texto_para_ids(pedido) + [CODIGO] + texto_para_ids(codigo) + [RESPOSTA]


def alvo_editar(procurar, substituir):
    return [EDITAR, PROCURAR] + texto_para_ids(procurar) + [SUBSTITUIR] + texto_para_ids(substituir) + [FIM]


def alvo_esclarecer(texto):
    return [ESCLARECER] + texto_para_ids(texto) + [FIM]


def alvo_recusar(texto):
    return [RECUSAR] + texto_para_ids(texto) + [FIM]


def interpretar(ids):
    """ids gerados -> dict {tipo: editar|esclarecer|recusar, ...} ou None se o formato for inválido.
    Estrito: um único bloco, terminado por FIM, sem marcadores fora do lugar, sem texto depois do FIM."""
    ids = list(ids)
    if FIM not in ids:
        return None
    ids = ids[:ids.index(FIM) + 1]
    corpo = ids[:-1]
    if any(i in (BOS, PAD, EOS, PEDIDO, CODIGO, RESPOSTA) for i in corpo):
        return None
    if corpo[:2] == [EDITAR, PROCURAR]:
        r = corpo[2:]
        if r.count(SUBSTITUIR) != 1 or any(i >= 256 and i != SUBSTITUIR for i in r):
            return None
        k = r.index(SUBSTITUIR)
        proc, subs = ids_para_texto(r[:k]), ids_para_texto(r[k + 1:])
        if not proc or proc == subs:
            return None
        return {"tipo": "editar", "procurar": proc, "substituir": subs}
    if corpo[:1] in ([ESCLARECER], [RECUSAR]):
        r = corpo[1:]
        if any(i >= 256 for i in r) or not r:
            return None
        return {"tipo": "esclarecer" if corpo[0] == ESCLARECER else "recusar", "texto": ids_para_texto(r)}
    return None


def identidade():
    """hash estável do tokenizer (entra no checkpoint)"""
    return hashlib.sha256(("%s|%d|%s" % (VERSAO, VOCAB, ",".join(NOMES))).encode()).hexdigest()
