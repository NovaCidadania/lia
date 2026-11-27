"""Tokenizador próprio para código + português: pré-tokenização léxica (identificadores, números, operadores compostos, espaços/indentação)
seguida de BPE treinado SÓ no corpus de treino do projeto, com base = 256 bytes (byte fallback: qualquer texto UTF-8 é representável).
Cada token carrega o offset em caracteres, para apontar o token de código que uma troca atômica altera."""
import hashlib, json, re
from collections import Counter

PRE = re.compile(r"[A-Za-zÀ-ÿ_]\w*|\d+|==|!=|<=|>=|\+=|-=|\*=|//|\n[ \t]*|[ \t]+|[^\s\w]|\s", re.U)


def pretokens(texto):
    return [(m.group(), m.start()) for m in PRE.finditer(texto)]


class BPE:
    def __init__(self, merges=None):
        self.merges = [tuple(m) for m in (merges or [])]
        self.rank = {m: i for i, m in enumerate(self.merges)}
        self.vocab_base = 256 + len(self.merges)
        self._cache = {}

    @property
    def n_vocab(self):
        return self.vocab_base

    @classmethod
    def treinar(cls, textos, n_merges=1500, min_freq=2):
        cont = Counter()
        for t in textos:
            for w, _ in pretokens(t):
                cont[w] += 1
        palavras = {w: list(w.encode("utf-8")) for w in cont}
        merges = []
        for _ in range(n_merges):
            pares = Counter()
            for w, seq in palavras.items():
                for a, b in zip(seq, seq[1:]):
                    pares[(a, b)] += cont[w]
            if not pares:
                break
            (par, f) = max(pares.items(), key=lambda kv: (kv[1], -kv[0][0], -kv[0][1]))
            if f < min_freq:
                break
            novo = 256 + len(merges); merges.append(par)
            for w, seq in palavras.items():
                i, out = 0, []
                while i < len(seq):
                    if i + 1 < len(seq) and (seq[i], seq[i + 1]) == par:
                        out.append(novo); i += 2
                    else:
                        out.append(seq[i]); i += 1
                palavras[w] = out
        return cls(merges)

    def _palavra(self, w):
        r = self._cache.get(w)
        if r is None:
            seq = list(w.encode("utf-8"))
            while len(seq) > 1:
                melhor = min(((self.rank.get(p, 1 << 30), i) for i, p in enumerate(zip(seq, seq[1:]))), default=None)
                if melhor is None or melhor[0] == 1 << 30:
                    break
                i = melhor[1]; seq[i:i + 2] = [256 + melhor[0]]
            r = self._cache[w] = tuple(seq)
        return r

    def codificar(self, texto):
        """-> (ids, offsets) com offset = (início, fim) em caracteres do PRÉ-TOKEN a que o token pertence."""
        ids, offs = [], []
        for w, ini in pretokens(texto):
            for t in self._palavra(w):
                ids.append(t); offs.append((ini, ini + len(w)))
        return ids, offs

    def identidade(self):
        return hashlib.sha256(("lia-bpe/1;" + json.dumps(self.merges)).encode()).hexdigest()
