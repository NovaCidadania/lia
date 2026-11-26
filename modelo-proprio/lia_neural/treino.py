"""Treino do zero, em CPU, com limites configuráveis (passos, tempo, memória, tamanho do dataset) e retomada de checkpoint.
A amostragem do lote depende só de (semente, passo): retomar de um checkpoint reproduz a mesma sequência de lotes."""
import json, math, os, random, resource, time

import torch
import torch.nn.functional as F

from . import tokenizer as tk
from .checkpoint import carregar, salvar
from .modelo import LiaGPT


def codificar(ex):
    """exemplo -> (ids, inicio_do_alvo). O alvo é a única parte com perda."""
    p = tk.montar_prompt(ex["pedido"], ex["codigo"])
    a = ex["alvo"]
    t = tk.alvo_editar(a["procurar"], a["substituir"]) if ex["tipo"] == "editar" else \
        tk.alvo_esclarecer(a["texto"]) if ex["tipo"] == "esclarecer" else tk.alvo_recusar(a["texto"])
    return p + t, len(p)


def preparar(exemplos, contexto):
    ok, descartados = [], 0
    for e in exemplos:
        ids, ini = codificar(e)
        if len(ids) > contexto:
            descartados += 1
        else:
            ok.append((ids, ini))
    return ok, descartados


def lote(itens):
    n = max(len(i) for i, _ in itens)
    x = torch.full((len(itens), n), tk.PAD, dtype=torch.long)
    m = torch.zeros((len(itens), n), dtype=torch.bool)
    for r, (ids, ini) in enumerate(itens):
        x[r, :len(ids)] = torch.tensor(ids)
        m[r, ini:len(ids)] = True        # perda só nos tokens do alvo (incluindo o FIM)
    return x, m


def perda(modelo, x, m):
    lg = modelo(x[:, :-1])
    alvo = x[:, 1:]
    mk = m[:, 1:]
    l = F.cross_entropy(lg.reshape(-1, lg.shape[-1]), alvo.reshape(-1), reduction="none").view_as(alvo)
    return (l * mk).sum() / mk.sum().clamp(min=1)


@torch.no_grad()
def perda_media(modelo, itens, bs=32):
    modelo.eval()
    tot, n = 0.0, 0
    for i in range(0, len(itens), bs):
        x, m = lote(itens[i:i + bs])
        k = int(m[:, 1:].sum())
        tot += float(perda(modelo, x, m)) * k
        n += k
    modelo.train()
    return tot / max(n, 1)


def rss_mb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0


def treinar(ds, man, saida, *, config=None, semente=1, passos=3000, lote_tam=24, lr=2e-3, warmup=100, max_segundos=None,
            max_memoria_mb=None, max_exemplos=None, salvar_cada=500, avaliar_cada=250, retomar=None, threads=None, log=print):
    """-> dict com resumo (perda inicial/final, checkpoints, tempo, memória). Levanta RuntimeError se estourar memória."""
    if threads:
        torch.set_num_threads(threads)
    torch.manual_seed(semente)
    ckpt = os.path.join(saida, "ultimo.pt")
    if retomar:
        modelo, d = carregar(retomar)
        if d["hash_dataset"] != man["hash_conjunto"]:
            raise RuntimeError("o checkpoint foi treinado com outro dataset (%s…) — recusado" % d["hash_dataset"][:12])
        passo0, historico, opt_estado, semente = d["passo"], d["historico"], d["otimizador"], d["semente"]
    else:
        modelo, passo0, historico, opt_estado = LiaGPT(config), 0, [], None
    modelo.train()
    treino, desc = preparar(ds["treino"][:max_exemplos] if max_exemplos else ds["treino"], modelo.config["contexto"])
    dev, _ = preparar(ds["dev"], modelo.config["contexto"])
    if not treino:
        raise RuntimeError("dataset de treino vazio")
    opt = torch.optim.AdamW(modelo.parameters(), lr=lr, betas=(0.9, 0.95), weight_decay=0.01)
    if opt_estado:
        opt.load_state_dict(opt_estado)

    def lr_em(p):
        if p < warmup:
            return lr * (p + 1) / warmup
        return lr * (0.1 + 0.9 * 0.5 * (1 + math.cos(math.pi * (p - warmup) / max(1, passos - warmup))))

    os.makedirs(saida, exist_ok=True)
    logf = open(os.path.join(saida, "treino.jsonl"), "a")
    t0 = time.time()
    inicial = None
    ultima_dev = None
    janela = []
    meta = None
    motivo = "passos"
    for p in range(passo0, passos):
        rng = random.Random("%d|%d" % (semente, p))
        x, m = lote([treino[rng.randrange(len(treino))] for _ in range(lote_tam)])
        for g in opt.param_groups:
            g["lr"] = lr_em(p)
        opt.zero_grad(set_to_none=True)
        l = perda(modelo, x, m)
        if not torch.isfinite(l):
            raise RuntimeError("perda não finita no passo %d" % p)
        l.backward()
        torch.nn.utils.clip_grad_norm_(modelo.parameters(), 1.0)
        opt.step()
        v = float(l)
        inicial = v if inicial is None else inicial
        janela.append(v)
        passo = p + 1
        if passo % 50 == 0 or passo == passos:
            med = sum(janela) / len(janela); janela = []
            reg = {"passo": passo, "perda_treino": round(med, 4), "lr": round(lr_em(p), 6), "t": round(time.time() - t0, 1), "rss_mb": round(rss_mb())}
            if passo % avaliar_cada == 0 or passo == passos:
                ultima_dev = perda_media(modelo, dev[:128]) if dev else None
                reg["perda_dev"] = round(ultima_dev, 4) if ultima_dev is not None else None
            historico.append(reg)
            logf.write(json.dumps(reg) + "\n"); logf.flush()
            log(json.dumps(reg))
        if passo % salvar_cada == 0 and passo < passos:
            meta = salvar(ckpt, modelo, semente=semente, hash_dataset=man["hash_conjunto"], passo=passo, historico=historico, otimizador=opt.state_dict())
        if max_memoria_mb and rss_mb() > max_memoria_mb:
            motivo = "memoria"
            salvar(ckpt, modelo, semente=semente, hash_dataset=man["hash_conjunto"], passo=passo, historico=historico, otimizador=opt.state_dict())
            raise RuntimeError("limite de memória excedido: %.0f MB > %d MB (checkpoint do passo %d salvo)" % (rss_mb(), max_memoria_mb, passo))
        if max_segundos and time.time() - t0 >= max_segundos:
            motivo = "tempo"
            break
    meta = salvar(ckpt, modelo, semente=semente, hash_dataset=man["hash_conjunto"], passo=passo, historico=historico, otimizador=opt.state_dict(),
                  extras={"motivo_parada": motivo})
    final_dev = perda_media(modelo, dev) if dev else None
    return {"passo_final": passo, "motivo_parada": motivo, "perda_inicial": inicial, "perda_treino_final": historico[-1]["perda_treino"] if historico else None,
            "perda_dev_final": final_dev, "exemplos_descartados_por_contexto": desc, "segundos": round(time.time() - t0, 1),
            "memoria_max_mb": round(rss_mb()), "checkpoint": ckpt, "meta": meta, "parametros": modelo.n_parametros()}
