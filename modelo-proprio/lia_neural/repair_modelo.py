"""V2: Transformer codificador de bytes, do zero (pesos aleatórios), que PONTUA trocas atômicas do arquivo.
Entrada: [CLS] pedido [SEP] código. Candidato = (posição do token no código, token antigo, token novo). Sem texto livre na saída."""
import hashlib

import torch
import torch.nn as nn

from . import repair_dados as RD

PAD, CLS, SEP, VOCAB = 256, 257, 258, 259
FORMATO = "lia-repair-checkpoint/1"
VERSAO = "lia-repair/2"
CONFIG_PADRAO = {"vocab": VOCAB, "contexto": 640, "d": 160, "camadas": 5, "cabecas": 5, "ff": 4, "dropout": 0.1, "ntok": len(RD.TOKS) + 1}


def identidade():
    return hashlib.sha256(("lia-repair-tokenizer/1;bytes+CLS,SEP,PAD;vocab=259;trocas=%s" % ",".join(RD.TOKS)).encode()).hexdigest()


def codificar(pedido, codigo, cands, contexto=640):
    """-> (ids, [(pos_ini, pos_fim)], [(tok_antigo, tok_novo)]) ou None se não cabe no contexto (nunca trunca em silêncio)."""
    cab = [CLS] + list(pedido.encode()) + [SEP]
    ids = cab + list(codigo.encode())
    if len(ids) > contexto:
        return None
    pos = [(len(cab) + c["ini"], len(cab) + c["fim"] - 1) for c in cands]
    toks = [(RD.tokid(c["antigo"]), RD.tokid(c["novo"])) for c in cands]
    return ids, pos, toks


class Localizador(nn.Module):
    def __init__(self, config=None):
        super().__init__()
        c = dict(CONFIG_PADRAO); c.update(config or {})
        self.config = c
        d = c["d"]
        self.tok, self.pos = nn.Embedding(c["vocab"], d, padding_idx=PAD), nn.Embedding(c["contexto"], d)
        cam = nn.TransformerEncoderLayer(d, c["cabecas"], c["ff"] * d, c["dropout"], activation="gelu", batch_first=True, norm_first=True)
        self.enc = nn.TransformerEncoder(cam, c["camadas"], enable_nested_tensor=False)
        self.norm = nn.LayerNorm(d)
        self.troca = nn.Embedding(c["ntok"], d)
        self.cabeca = nn.Sequential(nn.Linear(4 * d, 2 * d), nn.GELU(), nn.Linear(2 * d, 1))
        self.apply(self._init)

    @staticmethod
    def _init(m):
        if isinstance(m, (nn.Linear, nn.Embedding)):
            nn.init.normal_(m.weight, std=0.02)
            if getattr(m, "bias", None) is not None:
                nn.init.zeros_(m.bias)

    def forward(self, ids, lens, pos, toks, mask):
        """ids [B,T]; pos [B,C,2]; toks [B,C,2]; mask [B,C] -> scores [B,C] (inválidos = -1e9)."""
        b, t = ids.shape
        x = self.tok(ids) + self.pos(torch.arange(t, device=ids.device)[None])
        pad = torch.arange(t, device=ids.device)[None] >= lens[:, None]
        h = self.norm(self.enc(x, src_key_padding_mask=pad))
        g = lambda p: torch.gather(h, 1, p[..., None].expand(-1, -1, h.shape[-1]))  # noqa: E731
        z = torch.cat([g(pos[..., 0]), g(pos[..., 1]), self.troca(toks[..., 0]), self.troca(toks[..., 1])], -1)
        return self.cabeca(z).squeeze(-1).masked_fill(~mask, -1e9)

    def n_parametros(self):
        return sum(p.numel() for p in self.parameters())
