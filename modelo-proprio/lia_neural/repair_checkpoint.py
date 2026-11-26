"""Checkpoint do localizador V2: pesos, config, versão do código, tokenizer, semente, hash do dataset (+ otimizador). Atômico, com .json
de sha256. Carregar valida tudo; ausente/corrompido/de outra versão FALHA (CheckpointInvalido) — nunca recai em outro modelo."""
import hashlib, json, os, tempfile

import torch

from . import repair_modelo as M
from .checkpoint import CheckpointInvalido, sha256_arquivo


def salvar(caminho, modelo, *, semente, hash_dataset, passo, historico, otimizador=None, extras=None):
    d = {"formato": M.FORMATO, "versao_codigo": M.VERSAO, "tokenizer": M.identidade(), "semente": semente, "hash_dataset": hash_dataset,
         "config": modelo.config, "passo": passo, "historico": historico, "pesos": {k: v.detach().cpu() for k, v in modelo.state_dict().items()},
         "otimizador": otimizador, "extras": extras or {}}
    dest = os.path.dirname(os.path.abspath(caminho)); os.makedirs(dest, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=dest, suffix=".tmp"); os.close(fd)
    try:
        torch.save(d, tmp); os.replace(tmp, caminho)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    meta = {"arquivo": os.path.basename(caminho), "sha256": sha256_arquivo(caminho), "bytes": os.path.getsize(caminho), "parametros": modelo.n_parametros(),
            "passo": passo, "semente": semente, "hash_dataset": hash_dataset, "versao_codigo": M.VERSAO, "config": modelo.config}
    with open(caminho + ".json", "w") as f:
        json.dump(meta, f, indent=1)
    return meta


def formato_de(caminho):
    """'repair' se o arquivo é um checkpoint do localizador (usado pelo despacho do CLI); None se ilegível."""
    try:
        return "repair" if torch.load(caminho, map_location="cpu", weights_only=True).get("formato") == M.FORMATO else "v1"
    except Exception:  # noqa: BLE001
        return None


def carregar(caminho):
    if not caminho or not os.path.isfile(caminho):
        raise CheckpointInvalido("checkpoint ausente: %r" % (caminho,))
    if os.path.isfile(caminho + ".json") and json.load(open(caminho + ".json")).get("sha256") != sha256_arquivo(caminho):
        raise CheckpointInvalido("sha256 do checkpoint não confere com %s.json (arquivo alterado ou corrompido)" % caminho)
    try:
        d = torch.load(caminho, map_location="cpu", weights_only=True)
    except Exception as e:  # noqa: BLE001
        raise CheckpointInvalido("checkpoint ilegível: %s" % e)
    for k in ("formato", "versao_codigo", "tokenizer", "config", "pesos", "semente", "hash_dataset"):
        if k not in d:
            raise CheckpointInvalido("checkpoint sem o campo %r" % k)
    if d["formato"] != M.FORMATO or d["versao_codigo"] != M.VERSAO:
        raise CheckpointInvalido("formato/versão de checkpoint desconhecidos: %r %r" % (d["formato"], d["versao_codigo"]))
    if d["tokenizer"] != M.identidade():
        raise CheckpointInvalido("checkpoint de outro tokenizer")
    m = M.Localizador(d["config"])
    try:
        m.load_state_dict(d["pesos"], strict=True)
    except Exception as e:  # noqa: BLE001
        raise CheckpointInvalido("pesos incompatíveis: %s" % str(e)[:200])
    return m, d
