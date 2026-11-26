"""Decoder-only (GPT pré-norma) pequeno, treinado do ZERO: embeddings aprendidos, atenção causal, MLP GELU, saída com pesos
compartilhados. Inicialização aleatória com semente; nenhuma base pré-treinada."""
import math, warnings
warnings.filterwarnings("ignore", message=".*NumPy.*")
import torch
import torch.nn as nn
import torch.nn.functional as F

from . import tokenizer as tk

CONFIG_PADRAO = dict(vocab=tk.VOCAB, contexto=512, d=160, camadas=5, cabecas=5, mlp=4, dropout=0.0)


class Bloco(nn.Module):
    def __init__(self, c):
        super().__init__()
        d = c["d"]
        self.n1, self.n2 = nn.LayerNorm(d), nn.LayerNorm(d)
        self.qkv, self.proj = nn.Linear(d, 3 * d), nn.Linear(d, d)
        self.fc1, self.fc2 = nn.Linear(d, c["mlp"] * d), nn.Linear(c["mlp"] * d, d)
        self.h, self.drop = c["cabecas"], c["dropout"]

    def forward(self, x, passado=None, usar_cache=False):
        B, T, D = x.shape
        q, k, v = self.qkv(self.n1(x)).split(D, dim=2)
        q, k, v = (t.view(B, T, self.h, D // self.h).transpose(1, 2) for t in (q, k, v))
        if passado is not None:                      # decodificação incremental: 1 token novo vê todo o passado
            k, v = torch.cat([passado[0], k], dim=2), torch.cat([passado[1], v], dim=2)
            a = F.scaled_dot_product_attention(q, k, v)
        else:
            a = F.scaled_dot_product_attention(q, k, v, is_causal=True, dropout_p=self.drop if self.training else 0.0)
        x = x + self.proj(a.transpose(1, 2).reshape(B, T, D))
        x = x + self.fc2(F.gelu(self.fc1(self.n2(x))))
        return (x, (k, v)) if usar_cache else x


class LiaGPT(nn.Module):
    def __init__(self, config=None):
        super().__init__()
        c = dict(CONFIG_PADRAO)
        c.update(config or {})
        assert c["d"] % c["cabecas"] == 0
        self.config = c
        self.tok = nn.Embedding(c["vocab"], c["d"])
        self.pos = nn.Embedding(c["contexto"], c["d"])
        self.blocos = nn.ModuleList(Bloco(c) for _ in range(c["camadas"]))
        self.nf = nn.LayerNorm(c["d"])
        self.apply(self._init)
        for n, p in self.named_parameters():       # projeções residuais menores (GPT-2)
            if n.endswith("proj.weight") or n.endswith("fc2.weight"):
                nn.init.normal_(p, 0.0, 0.02 / math.sqrt(2 * c["camadas"]))

    @staticmethod
    def _init(m):
        if isinstance(m, nn.Linear):
            nn.init.normal_(m.weight, 0.0, 0.02)
            nn.init.zeros_(m.bias)
        elif isinstance(m, nn.Embedding):
            nn.init.normal_(m.weight, 0.0, 0.02)

    def forward(self, ids, passado=None, usar_cache=False):
        T = ids.shape[1]
        p0 = 0 if passado is None else passado[0][0].shape[2]
        x = self.tok(ids) + self.pos(torch.arange(p0, p0 + T, device=ids.device))
        novo = []
        for i, b in enumerate(self.blocos):
            if usar_cache:
                x, kv = b(x, None if passado is None else passado[i], True)
                novo.append(kv)
            else:
                x = b(x)
        lg = self.nf(x) @ self.tok.weight.T        # pesos de saída = embeddings (compartilhados)
        return (lg, novo) if usar_cache else lg

    def n_parametros(self):
        return sum(p.numel() for p in self.parameters())

    @torch.no_grad()
    def gerar(self, prompt_ids, max_novos=220):
        """decodificação gulosa (temperatura 0), para em FIM, no limite de tokens ou no contexto"""
        self.eval()
        ids = list(prompt_ids)
        novos = []
        passado = None
        entrada = torch.tensor([ids])
        for _ in range(max_novos):
            if len(ids) >= self.config["contexto"]:
                break
            lg, passado = self(entrada, passado, True)
            lg = lg[0, -1]
            lg[tk.PAD] = lg[tk.BOS] = lg[tk.EOS] = lg[tk.PEDIDO] = lg[tk.CODIGO] = lg[tk.RESPOSTA] = -1e9
            t = int(lg.argmax())
            ids.append(t); novos.append(t)
            entrada = torch.tensor([[t]])
            if t == tk.FIM:
                break
        return novos
