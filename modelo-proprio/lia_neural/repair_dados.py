"""V2: dados do localizador. Cada exemplo = arquivo com UM defeito injetado por troca atômica de token + pedido + testes.
O alvo é a troca inversa (índice em `candidatos`). Tudo sintético e nosso (sem dado de terceiros)."""
import hashlib, io, json, random, re, sys, tokenize

from . import dados as D
from .familias import FAMILIAS, NOMES
from .familias2 import FAMILIAS2
from .familias3 import familias_composicionais
from .familias4 import familias_expressao

N_COMP = 200
N_EXPR = 600
COMP = familias_composicionais(N_COMP) + familias_expressao(N_EXPR)

VERSAO = "lia-repair-dados/2"
GRUPOS = [("+", "-", "*"), ("<", "<=", ">", ">="), ("+=", "-=", "*="), ("//", "%"), ("==", "!="), ("and", "or"), ("min", "max"), ("True", "False")]
TROCAS = {t: tuple(x for x in g if x != t) for g in GRUPOS for t in g}
TOKS = sorted(TROCAS) + [str(i) for i in range(0, 101)]
TOK_ID = {t: i + 1 for i, t in enumerate(TOKS)}          # 0 = desconhecido
MAX_TOTAL = 600                                          # pedido + código, em bytes
POOLS = {"total": ("total", "soma", "acc", "res", "s"), "v": ("v", "x", "item", "e", "valor"), "r": ("r", "res", "prod", "acc"),
         "m": ("m", "melhor", "atual", "mx"), "ch": ("ch", "c", "letra"), "i": ("i", "k2", "j", "idx"), "passos": ("passos", "cont", "t")}


class _Passos(Exception):
    pass


def _ex(src, nome, args, max_linhas=30000):
    """Executa nome(*args) com limite DETERMINÍSTICO de linhas executadas (não de relógio): mutantes com laço infinito caem sempre
    no mesmo ponto, então o dataset e a avaliação são reproduzíveis byte a byte mesmo com a máquina carregada."""
    n = [0]

    def traco(frame, ev, arg):
        if ev == "line":
            n[0] += 1
            if n[0] > max_linhas:
                raise _Passos()
        return traco
    try:
        ns = {}
        exec(src, ns)
        a = [list(x) if isinstance(x, list) else x for x in args]
        sys.settrace(traco)
        try:
            return ("ok", ns[nome](*a))
        finally:
            sys.settrace(None)
    except _Passos:
        return ("erro", "Tempo")
    except Exception as e:  # noqa: BLE001 — defeito injetado pode lançar qualquer coisa
        return ("erro", type(e).__name__)


def tokid(s):
    return TOK_ID.get(s, 0)


def candidatos(codigo):
    """Trocas atômicas do arquivo. Cada item: c0/c1 (offsets em caracteres), ini/fim (em bytes), antigo, novo, procurar, substituir (linha
    inteira, que precisa ser única no arquivo)."""
    linhas = codigo.splitlines(True)
    inicio, n = [], 0
    for l in linhas:
        inicio.append(n); n += len(l)
    out = []
    try:
        for t in tokenize.generate_tokens(io.StringIO(codigo).readline):
            if t.type not in (tokenize.OP, tokenize.NAME, tokenize.NUMBER):
                continue
            s = t.string
            if t.type == tokenize.NUMBER:
                if not s.isdigit() or len(s) > 2:
                    continue
                v = int(s)
                novos = [str(v + 1)] + ([str(v - 1)] if v >= 1 else [])
            elif s in TROCAS:
                novos = list(TROCAS[s])
            else:
                continue
            c0 = inicio[t.start[0] - 1] + t.start[1]
            c1 = inicio[t.end[0] - 1] + t.end[1]
            linha = linhas[t.start[0] - 1].rstrip("\n")
            if codigo.count(linha) != 1:
                continue
            for nv in novos:
                col = t.start[1]
                out.append({"c0": c0, "c1": c1, "ini": len(codigo[:c0].encode()), "fim": len(codigo[:c1].encode()),
                            "antigo": s, "novo": nv, "procurar": linha, "substituir": linha[:col] + nv + linha[t.end[1]:]})
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return []
    return out


def aplicar(codigo, c):
    return codigo[:c["c0"]] + c["novo"] + codigo[c["c1"]:]


def _renomear(src, r):
    usados = set(re.findall(r"[A-Za-z_]\w*", src))
    for base, pool in POOLS.items():
        if base not in usados or r.random() < 0.4:
            continue
        novo = r.choice([p for p in pool if p == base or p not in usados] or [base])
        if novo != base:
            src = re.sub(r"(?<![\w.'])%s(?![\w'])" % re.escape(base), novo, src); usados.add(novo)
    return src


def _exemplo(r, fam, outras, ident):
    nome = r.choice(NOMES)
    bom_f = _renomear(fam["src"], r).replace("{f}", nome)
    cb = candidatos(bom_f)
    if not cb:
        return None
    c = r.choice(cb)
    ruim_f = aplicar(bom_f, c)
    extra = ""
    if outras and r.random() < 0.4:
        o = r.choice(outras)
        extra = o["src"].replace("{f}", r.choice([n for n in NOMES if n != nome])) + "\n"
    antes = r.random() < 0.5
    deslocamento = len(extra) if antes else 0
    bom = (extra + bom_f) if antes else (bom_f + ("\n" + extra if extra else ""))
    ruim = (extra + ruim_f) if antes else (ruim_f + ("\n" + extra if extra else ""))
    if len(ruim.encode()) > D.MAX_BYTES_CODIGO:
        return None
    cands = candidatos(ruim)
    alvo = [i for i, k in enumerate(cands) if k["c0"] == c["c0"] + deslocamento and k["novo"] == c["antigo"]]
    if len(alvo) != 1 or len(cands) > 60:
        return None
    entradas, vistos = [], set()
    for _ in range(400):
        e = D._entrada(fam["entrada"], r)
        k = json.dumps(e)
        if k not in vistos:
            vistos.add(k); entradas.append(e)
        if len(entradas) >= 30:
            break
    dif = [e for e in entradas if _ex(bom_f, nome, e) != _ex(ruim_f, nome, e)]
    if len(dif) < 3 or len(entradas) < D.N_VISIVEIS + D.N_OCULTOS:
        return None
    sint = dif[0]
    rest = [e for e in entradas if e is not sint]
    vis = [sint] + rest[:D.N_VISIVEIS - 1]
    ocu = rest[D.N_VISIVEIS - 1:D.N_VISIVEIS - 1 + D.N_OCULTOS]
    if not any(_ex(bom_f, nome, e) != _ex(ruim_f, nome, e) for e in ocu):
        return None
    got, exp = D._repr_res(_ex(ruim_f, nome, sint)), D._repr_res(_ex(bom_f, nome, sint))
    pedido = r.choice(D.FRASES_EDITAR).format(f=nome, d=fam["d"], a=D._repr_args(sint), got=got, exp=exp)
    if len(pedido.encode()) + len(ruim.encode()) > MAX_TOTAL:
        return None
    return {"id": ident, "familia": fam["id"], "pedido": pedido, "codigo": ruim, "funcao": nome, "correto": bom,
            "candidatos": cands, "alvo": alvo[0], "visivel": vis, "oculto": ocu}


def familias_particionadas(semente=20261126):
    novas = [f["id"] for f in FAMILIAS2]
    random.Random(semente).shuffle(novas)
    return {"teste": sorted(novas[:18]), "dev": sorted(novas[18:26]),
            "treino": sorted([f["id"] for f in FAMILIAS] + novas[26:] + [f["id"] for f in COMP])}


def _por_id():
    return {f["id"]: f for f in FAMILIAS + FAMILIAS2 + COMP}


def gerar_particao(nome, n, semente, ids):
    r = random.Random("%s|%s|%s" % (VERSAO, nome, semente))
    pool = [_por_id()[i] for i in ids]
    out, tent = [], 0
    while len(out) < n:
        tent += 1
        if tent > n * 200:
            raise RuntimeError("não consegui gerar %d exemplos de %s" % (n, nome))
        e = _exemplo(r, r.choice(pool), pool, "%s-%05d" % (nome, len(out)))
        if e:
            out.append(e)
    return out


def hash_exemplos(ex):
    return hashlib.sha256(json.dumps(ex, ensure_ascii=False, sort_keys=True).encode()).hexdigest()


def gerar(semente=1, n_treino=8000, n_dev=400, n_teste=900):
    fam = familias_particionadas()
    ds = {k: gerar_particao(k, n, semente, fam[k]) for k, n in (("treino", n_treino), ("dev", n_dev), ("teste", n_teste))}
    man = {"formato": "lia-repair-dataset/2", "gerador": VERSAO, "semente": semente, "familias": fam,
           "licenca": "próprio (sintético, gerado pelo projeto; sem dado de terceiros nem de repositórios do usuário)",
           "particoes": {k: {"n": len(v), "hash": hash_exemplos(v), "familias": sorted({e["familia"] for e in v})} for k, v in ds.items()}}
    man["hash_conjunto"] = hashlib.sha256(json.dumps({k: v["hash"] for k, v in man["particoes"].items()}, sort_keys=True).encode()).hexdigest()
    return ds, man


def sem_vazamento(ds):
    erros, ks = [], list(ds)
    fam = {k: {e["familia"] for e in v} for k, v in ds.items()}
    for i, a in enumerate(ks):
        for b in ks[i + 1:]:
            if fam[a] & fam[b]:
                erros.append("famílias repetidas entre %s e %s" % (a, b))
            if {(e["pedido"], e["codigo"]) for e in ds[a]} & {(e["pedido"], e["codigo"]) for e in ds[b]}:
                erros.append("exemplos repetidos entre %s e %s" % (a, b))
    return erros


def funcao_span(codigo, nome):
    m = re.search(r"^def %s\(" % re.escape(nome), codigo, re.M)
    if not m:
        return None
    f = re.search(r"^def ", codigo[m.end():], re.M)
    return (m.start(), m.end() + (f.start() if f else len(codigo)))
