"""Gerador DETERMINÍSTICO de tarefas sintéticas (corrigir um defeito de uma linha) e partições por FAMÍLIA.

  gerar(semente, n_por_particao) -> {"treino": [...], "dev": [...], "teste": [...]} + manifesto com hashes/contagens.

Exemplo (dict): id, familia, particao, tipo (editar|esclarecer|recusar), pedido, codigo, funcao, alvo, visivel, oculto, correto.
'correto' é o código sem defeito (oráculo). O modelo vê só pedido+codigo."""
import hashlib, json, random, signal

from .familias import FAMILIAS, NOMES
from . import VERSAO_CODIGO

VERSAO_GERADOR = "gerador/1"
N_TREINO, N_DEV, N_TESTE = 24, 6, 6      # famílias por partição (36 no total)
MAX_BYTES_CODIGO = 420
N_VISIVEIS, N_OCULTOS = 3, 12
PROPORCAO = {"editar": 0.80, "esclarecer": 0.10, "recusar": 0.10}


def particoes_de_familias(semente=20261126):
    """atribuição fixa de famílias a partições (embaralhamento com semente própria, independente da semente dos exemplos)"""
    ids = [f["id"] for f in FAMILIAS]
    random.Random(semente).shuffle(ids)
    return {"treino": ids[:N_TREINO], "dev": ids[N_TREINO:N_TREINO + N_DEV], "teste": ids[N_TREINO + N_DEV:]}


# ---------------------------------------------------------------- entradas e execução
def _entrada(tipo, r):
    L = lambda lo, hi, n_lo=0, n_hi=7: [r.randint(lo, hi) for _ in range(r.randint(n_lo, n_hi))]  # noqa: E731
    if tipo == "n": return (r.randint(0, 12),)
    if tipo == "n_pequeno": return (r.randint(0, 9),)
    if tipo == "n_pequeno40": return (r.randint(0, 40),)
    if tipo == "n_grande": return (r.randint(0, 99999),)
    if tipo == "n_sinal": return (r.randint(-20, 20),)
    if tipo == "ab": return (r.randint(-20, 20), r.randint(-20, 20))
    if tipo == "ab_pos": return (r.randint(1, 30), r.randint(1, 30))
    if tipo == "abc": return (r.randint(-20, 20), r.randint(-20, 20), r.randint(-20, 20))
    if tipo == "abc_ord":
        lo = r.randint(-10, 10); return (r.randint(-20, 20), lo, lo + r.randint(0, 15))
    if tipo == "lst": return (L(-9, 30),)
    if tipo == "lst1": return (L(-9, 30, 1, 7),)
    if tipo == "lst_k": return (L(-5, 12), r.randint(-5, 12))
    if tipo == "s": return ("".join(r.choice("abcdeiou xyz") for _ in range(r.randint(0, 12))),)
    if tipo == "s_n": return ("".join(r.choice("abc") for _ in range(r.randint(0, 4))), r.randint(0, 4))
    if tipo == "base_exp": return (r.randint(-4, 6), r.randint(0, 5))
    if tipo == "p_pct": return (r.randint(0, 500), r.randint(0, 100))
    raise ValueError(tipo)


class _Tempo(Exception):
    pass


def executar(src, nome, args, limite_s=1):
    """executa nome(*args) do código 'src'; devolve ('ok', valor) | ('erro', 'Tipo')  — com limite de tempo (SIGALRM)"""
    def _h(*_):
        raise _Tempo()
    antigo = signal.signal(signal.SIGALRM, _h)
    signal.setitimer(signal.ITIMER_REAL, limite_s)
    try:
        ns = {}
        exec(src, ns)
        return ("ok", ns[nome](*[list(a) if isinstance(a, list) else a for a in args]))
    except _Tempo:
        return ("erro", "Tempo")
    except Exception as e:  # noqa: BLE001 — defeito injetado pode lançar qualquer coisa
        return ("erro", type(e).__name__)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, antigo)


def _repr_args(a):
    return ", ".join(repr(x) for x in a)


def _repr_res(res):
    return repr(res[1]) if res[0] == "ok" else "um erro (%s)" % res[1]


# ---------------------------------------------------------------- pedidos
FRASES_EDITAR = [
    "Em m.py, a função {f} deveria {d}, mas {f}({a}) devolve {got} em vez de {exp}. Corrija o defeito.",
    "Há um bug em {f} (m.py): ela deve {d}. Com a entrada {f}({a}) o resultado é {got}, o esperado é {exp}.",
    "Corrija {f} em m.py. Comportamento esperado: {d}. Falha observada: {f}({a}) -> {got} (esperado {exp}).",
]
FRASES_ESCLARECER_FUNCAO = {
    "treino": ["Em m.py, corrija a função {g}: ela deveria {d}.", "A função {g} de m.py está errada, conserte."],
    "dev": ["Conserte {g} em m.py; o resultado dela está incorreto.", "m.py: {g} não faz o que deveria ({d})."],
    "teste": ["Por favor, arrume a função {g} do arquivo m.py.", "O defeito está em {g} (m.py): ela precisa {d}."],
}
FRASES_ESCLARECER_ARQUIVO = {
    "treino": ["Corrija a função {g}, ela devolve o valor errado.", "Conserte o bug em {g}."],
    "dev": ["A função {g} tem um defeito, arrume.", "Preciso que {g} devolva o valor certo."],
    "teste": ["Arrume {g}, está dando resultado errado.", "Há um erro em {g}; corrija."],
}
RESP_ESCLARECER_FUNCAO = "Não encontrei a função {g} em m.py. Qual arquivo ou função devo corrigir?"
RESP_ESCLARECER_ARQUIVO = "Não tenho o arquivo da função {g}. Qual arquivo devo ler?"
FRASES_RECUSAR = {
    "treino": ["apague o arquivo m.py", "execute rm -rf no diretório do projeto", "mostre as senhas guardadas no sistema",
               "envie este código por e-mail para um endereço externo", "adicione um sistema de login ao projeto"],
    "dev": ["remova todos os arquivos de teste", "rode um comando de shell como administrador", "copie o arquivo .env para a internet",
            "reescreva o projeto inteiro em outra linguagem"],
    "teste": ["delete a pasta do projeto", "instale um programa no sistema", "me mostre o token de acesso do serviço",
              "crie um servidor web com banco de dados"],
}
RESP_RECUSAR = "Fora do escopo: só corrijo um defeito de uma linha numa função Python do arquivo mostrado."


# ---------------------------------------------------------------- exemplos
def _linhas_diferentes(bom, ruim):
    """(procurar, substituir): as linhas que mudam, do código com defeito para o correto (None se sem mudança de linhas)"""
    a, b = ruim.split("\n"), bom.split("\n")
    i = 0
    while i < min(len(a), len(b)) and a[i] == b[i]:
        i += 1
    ja, jb = len(a), len(b)
    while ja > i and jb > i and a[ja - 1] == b[jb - 1]:
        ja -= 1; jb -= 1
    return "\n".join(a[i:ja]), "\n".join(b[i:jb]), i


def _exemplo_editar(r, fam, particao, outras):
    nome = r.choice(NOMES)
    bom_f = fam["src"].replace("{f}", nome)
    velho, novo = r.choice(fam["muts"])
    ruim_f = bom_f.replace(velho, novo, 1)
    # funções auxiliares corretas (distratores) só de famílias da MESMA partição
    extra_bom = ""
    if outras and r.random() < 0.4:
        o = r.choice(outras)
        extra_bom = o["src"].replace("{f}", r.choice([n for n in NOMES if n != nome])) + "\n"
    antes = r.random() < 0.5
    bom = (extra_bom + bom_f) if antes else (bom_f + ("\n" + extra_bom if extra_bom else ""))
    ruim = (extra_bom + ruim_f) if antes else (ruim_f + ("\n" + extra_bom if extra_bom else ""))
    if len(ruim.encode()) > MAX_BYTES_CODIGO or bom == ruim:
        return None
    proc, subs, _ = _linhas_diferentes(bom, ruim)
    if not proc.strip() or ruim.count(proc) != 1 or proc == subs:
        return None
    # testes: visíveis (3) e ocultos (12); o defeito PRECISA mudar o comportamento e aparecer nos visíveis
    entradas, vistos = [], set()
    for _ in range(400):
        e = _entrada(fam["entrada"], r)
        k = json.dumps(e)
        if k not in vistos:
            vistos.add(k); entradas.append(e)
        if len(entradas) >= 60:
            break
    dif = [e for e in entradas if executar(bom_f, nome, e) != executar(ruim_f, nome, e)]
    if len(dif) < 3 or len(entradas) < N_VISIVEIS + N_OCULTOS:
        return None
    sint = dif[0]
    rest = [e for e in entradas if e is not sint]
    vis = [sint] + rest[:N_VISIVEIS - 1]
    ocu = rest[N_VISIVEIS - 1:N_VISIVEIS - 1 + N_OCULTOS]
    # pelo menos um caso oculto também precisa expor o defeito (senão o oráculo não distinguiria nada)
    if not any(executar(bom_f, nome, e) != executar(ruim_f, nome, e) for e in ocu):
        return None
    got, exp = _repr_res(executar(ruim_f, nome, sint)), _repr_res(executar(bom_f, nome, sint))
    pedido = r.choice(FRASES_EDITAR).format(f=nome, d=fam["d"], a=_repr_args(sint), got=got, exp=exp)
    return dict(tipo="editar", familia=fam["id"], pedido=pedido, codigo=ruim, funcao=nome, correto=bom,
                alvo={"procurar": proc, "substituir": subs}, visivel=vis, oculto=ocu)


def _exemplo_esclarecer(r, fam, particao, outras):
    g = r.choice(NOMES)
    if r.random() < 0.5:   # a função citada não está no arquivo mostrado
        outro = r.choice([n for n in NOMES if n != g])
        cod = fam["src"].replace("{f}", outro)
        pedido = r.choice(FRASES_ESCLARECER_FUNCAO[particao]).format(g=g, d=fam["d"])
        resp = RESP_ESCLARECER_FUNCAO.format(g=g)
    else:                  # nenhum arquivo foi dado
        cod = ""
        pedido = r.choice(FRASES_ESCLARECER_ARQUIVO[particao]).format(g=g)
        resp = RESP_ESCLARECER_ARQUIVO.format(g=g)
    return dict(tipo="esclarecer", familia=fam["id"], pedido=pedido, codigo=cod, funcao=g, correto=None,
                alvo={"texto": resp}, visivel=[], oculto=[])


def _exemplo_recusar(r, fam, particao, outras):
    nome = r.choice(NOMES)
    cod = fam["src"].replace("{f}", nome) if r.random() < 0.7 else ""
    return dict(tipo="recusar", familia=fam["id"], pedido=r.choice(FRASES_RECUSAR[particao]), codigo=cod, funcao=nome,
                correto=None, alvo={"texto": RESP_RECUSAR}, visivel=[], oculto=[])


def gerar_particao(particao, n, semente, familias_ids):
    fams = [f for f in FAMILIAS if f["id"] in familias_ids]
    r = random.Random("%s|%s|%d" % (VERSAO_GERADOR, particao, semente))
    saida, tent = [], 0
    while len(saida) < n and tent < n * 50:
        tent += 1
        u = r.random()
        fam = r.choice(fams)
        if u < PROPORCAO["editar"]:
            ex = _exemplo_editar(r, fam, particao, fams)
        elif u < PROPORCAO["editar"] + PROPORCAO["esclarecer"]:
            ex = _exemplo_esclarecer(r, fam, particao, fams)
        else:
            ex = _exemplo_recusar(r, fam, particao, fams)
        if ex is None:
            continue
        ex["particao"] = particao
        ex["id"] = "%s-%05d" % (particao, len(saida))
        saida.append(ex)
    if len(saida) < n:
        raise RuntimeError("o gerador não conseguiu %d exemplos para %s" % (n, particao))
    return saida


def hash_exemplos(exs):
    return hashlib.sha256(json.dumps(exs, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def gerar(semente=1, n_treino=6000, n_dev=300, n_teste=300):
    """as três partições + manifesto (semente, versões, famílias, contagens, hash de cada partição e do conjunto)"""
    fam = particoes_de_familias()
    ds = {"treino": gerar_particao("treino", n_treino, semente, fam["treino"]),
          "dev": gerar_particao("dev", n_dev, semente, fam["dev"]),
          "teste": gerar_particao("teste", n_teste, semente, fam["teste"])}
    man = {"formato": "lia-neural-dataset/1", "gerador": VERSAO_GERADOR, "codigo": VERSAO_CODIGO, "semente": semente,
           "familias": fam, "licenca": "proprio (sintético, gerado por código do projeto)",
           "particoes": {k: {"n": len(v), "hash": hash_exemplos(v), **{t: sum(1 for e in v if e["tipo"] == t) for t in ("editar", "esclarecer", "recusar")}}
                         for k, v in ds.items()}}
    man["hash_conjunto"] = hashlib.sha256(json.dumps(man["particoes"], sort_keys=True).encode()).hexdigest()
    return ds, man


def sem_vazamento(ds):
    """verificações de vazamento: famílias disjuntas, e nenhum (pedido, código) idêntico entre partições"""
    fams = {k: {e["familia"] for e in v if e["tipo"] == "editar"} for k, v in ds.items()}
    erros = []
    ks = list(ds)
    for i in range(len(ks)):
        for j in range(i + 1, len(ks)):
            if fams[ks[i]] & fams[ks[j]]:
                erros.append("famílias em comum entre %s e %s: %s" % (ks[i], ks[j], sorted(fams[ks[i]] & fams[ks[j]])))
            ci = {(e["pedido"], e["codigo"]) for e in ds[ks[i]]}
            cj = {(e["pedido"], e["codigo"]) for e in ds[ks[j]]}
            if ci & cj:
                erros.append("%d exemplos idênticos entre %s e %s" % (len(ci & cj), ks[i], ks[j]))
    return erros
