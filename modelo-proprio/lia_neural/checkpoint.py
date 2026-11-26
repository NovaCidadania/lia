"""Checkpoint: pesos + configuração + versão do código + semente + hash do dataset + tokenizer (+ otimizador para retomar).
Gravado de forma atômica; ao lado vai um .json com o sha256 do arquivo. Carregar valida versão, tokenizer e sha256 — checkpoint
ausente, corrompido ou de outra versão FALHA com erro claro (nunca recai em outro modelo)."""
import hashlib, json, os, tempfile
import torch

from . import VERSAO_CODIGO, tokenizer as tk
from .modelo import LiaGPT


class CheckpointInvalido(Exception):
    pass


def sha256_arquivo(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def salvar(caminho, modelo, *, semente, hash_dataset, passo, historico, otimizador=None, extras=None):
    d = {"formato": "lia-neural-checkpoint/1", "versao_codigo": VERSAO_CODIGO, "tokenizer": tk.identidade(), "semente": semente,
         "hash_dataset": hash_dataset, "config": modelo.config, "passo": passo, "historico": historico,
         "pesos": {k: v.detach().cpu() for k, v in modelo.state_dict().items()},
         "otimizador": otimizador, "extras": extras or {}}
    dest = os.path.dirname(os.path.abspath(caminho))
    os.makedirs(dest, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=dest, suffix=".tmp")
    os.close(fd)
    try:
        torch.save(d, tmp)
        os.replace(tmp, caminho)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    meta = {"arquivo": os.path.basename(caminho), "sha256": sha256_arquivo(caminho), "bytes": os.path.getsize(caminho),
            "parametros": modelo.n_parametros(), "passo": passo, "semente": semente, "hash_dataset": hash_dataset,
            "versao_codigo": VERSAO_CODIGO, "config": modelo.config}
    with open(caminho + ".json", "w") as f:
        json.dump(meta, f, indent=1)
    return meta


def carregar(caminho, verificar_sha=True):
    """-> (modelo, dict do checkpoint). Levanta CheckpointInvalido com a causa."""
    if not caminho or not os.path.isfile(caminho):
        raise CheckpointInvalido("checkpoint ausente: %r" % (caminho,))
    if verificar_sha and os.path.isfile(caminho + ".json"):
        meta = json.load(open(caminho + ".json"))
        if meta.get("sha256") != sha256_arquivo(caminho):
            raise CheckpointInvalido("sha256 do checkpoint não confere com %s.json (arquivo alterado ou corrompido)" % caminho)
    try:
        d = torch.load(caminho, map_location="cpu", weights_only=True)
    except Exception as e:  # noqa: BLE001
        raise CheckpointInvalido("checkpoint ilegível: %s" % e)
    for k in ("formato", "versao_codigo", "tokenizer", "config", "pesos", "semente", "hash_dataset"):
        if k not in d:
            raise CheckpointInvalido("checkpoint sem o campo %r" % k)
    if d["formato"] != "lia-neural-checkpoint/1":
        raise CheckpointInvalido("formato de checkpoint desconhecido: %r" % d["formato"])
    if d["versao_codigo"] != VERSAO_CODIGO:
        raise CheckpointInvalido("checkpoint de outra versão do código (%s, esperado %s)" % (d["versao_codigo"], VERSAO_CODIGO))
    if d["tokenizer"] != tk.identidade():
        raise CheckpointInvalido("checkpoint de outro tokenizer")
    m = LiaGPT(d["config"])
    try:
        m.load_state_dict(d["pesos"], strict=True)
    except Exception as e:  # noqa: BLE001
        raise CheckpointInvalido("pesos incompatíveis com a configuração: %s" % str(e)[:200])
    return m, d
