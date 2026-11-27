"""Treino do localizador V2 (AdamW, aquecimento + cosseno, clip 1.0). O lote depende só de (semente, passo): retomar reproduz o mesmo treino.
Escolhe o melhor checkpoint por perda de DESENVOLVIMENTO (nunca olha teste). Limites: passos, segundos, memória, exemplos."""
import json, math, os, random, resource, time

import torch
import torch.nn.functional as F

from . import repair_checkpoint as C, repair_modelo as M
from .bpe import BPE


def rss_mb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0


def preparar(exemplos, contexto, bpe=None):
    out = []
    for e in exemplos:
        c = M.codificar(e["pedido"], e["codigo"], e["candidatos"], contexto, bpe)
        if c:
            out.append((c, e["alvo"]))
    if not out:
        raise RuntimeError("nenhum exemplo cabe no contexto")
    return out


def lote(itens, pad=M.PAD):
    b = len(itens); nc = max(len(c[1]) for c, _ in itens); nt = max(len(c[0]) for c, _ in itens)
    ids = torch.full((b, nt), pad, dtype=torch.long); lens = torch.zeros(b, dtype=torch.long)
    pos = torch.zeros((b, nc, 2), dtype=torch.long); toks = torch.zeros((b, nc, 2), dtype=torch.long)
    mask = torch.zeros((b, nc), dtype=torch.bool); alvo = torch.zeros(b, dtype=torch.long)
    for i, ((x, p, t), a) in enumerate(itens):
        ids[i, :len(x)] = torch.tensor(x); lens[i] = len(x); alvo[i] = a
        pos[i, :len(p)] = torch.tensor(p); toks[i, :len(t)] = torch.tensor(t); mask[i, :len(p)] = True
    return ids, lens, pos, toks, mask, alvo


@torch.no_grad()
def metricas(modelo, itens, tam=32):
    modelo.eval(); n = perda = top1 = top3 = 0
    for p in range(0, len(itens), tam):
        ids, lens, pos, toks, mask, alvo = lote(itens[p:p + tam], modelo.config.get("pad", M.PAD))
        s = modelo(ids, lens, pos, toks, mask)
        perda += float(F.cross_entropy(s, alvo, reduction="sum")); n += len(alvo)
        rk = s.argsort(dim=1, descending=True)
        top1 += int((rk[:, 0] == alvo).sum()); top3 += int((rk[:, :3] == alvo[:, None]).any(1).sum())
    modelo.train()
    return {"n": n, "perda": perda / n, "top1": top1 / n, "recall3": top3 / n}


def treinar(ds, man, saida, *, config=None, semente=1, passos=4000, lote_tam=16, lr=1e-3, warmup=150, avaliar_cada=250, paciencia=8,
            max_segundos=None, max_memoria_mb=None, max_exemplos=None, retomar=None, threads=None, tokenizador="bytes", n_merges=1500, log=print):
    if threads:
        torch.set_num_threads(threads)
    torch.manual_seed(semente); os.makedirs(saida, exist_ok=True)
    ultimo, melhor_p = os.path.join(saida, "ultimo.pt"), os.path.join(saida, "melhor.pt")
    if retomar:
        modelo, ant = C.carregar(retomar)
        if ant["hash_dataset"] != man["hash_conjunto"]:
            raise RuntimeError("checkpoint treinado com outro dataset")
        inicio, hist, semente, est = ant["passo"], ant["historico"], ant["semente"], ant.get("otimizador")
    else:
        config = dict(config or {})
        bpe = None
        if tokenizador == "bpe":
            bpe = BPE.treinar([e["pedido"] + "\n" + e["codigo"] for e in ds["treino"]], n_merges)      # só o corpus de TREINO
            config.update(vocab=bpe.n_vocab + 3, pad=bpe.n_vocab + 2, contexto=config.get("contexto", 320))
        modelo, inicio, hist, est = M.Localizador(config), 0, [], None
        modelo.bpe = bpe
    ctx = modelo.config["contexto"]
    treino = preparar(ds["treino"][:max_exemplos] if max_exemplos else ds["treino"], ctx, modelo.bpe)
    dev = preparar(ds["dev"], ctx, modelo.bpe)
    opt = torch.optim.AdamW(modelo.parameters(), lr=lr, betas=(0.9, 0.98), weight_decay=0.05)
    if est:
        opt.load_state_dict(est)
    taxa = lambda p: lr * (p + 1) / warmup if p < warmup else lr * (0.1 + 0.9 * 0.5 * (1 + math.cos(math.pi * (p - warmup) / max(1, passos - warmup))))  # noqa: E731
    melhor, ruim, motivo, passo_f, janela, t0 = (-1.0, float("inf")), 0, "passos", inicio, [], time.time()
    modelo.train()
    with open(os.path.join(saida, "treino.jsonl"), "a") as arq:
        for p in range(inicio, passos):
            rng = random.Random("%s|%s" % (semente, p))
            ids, lens, pos, toks, mask, alvo = lote([treino[rng.randrange(len(treino))] for _ in range(lote_tam)], modelo.config.get("pad", M.PAD))
            for g in opt.param_groups:
                g["lr"] = taxa(p)
            opt.zero_grad(set_to_none=True)
            perda = F.cross_entropy(modelo(ids, lens, pos, toks, mask), alvo)
            if not torch.isfinite(perda):
                raise RuntimeError("perda não finita no passo %d" % p)
            perda.backward(); torch.nn.utils.clip_grad_norm_(modelo.parameters(), 1.0); opt.step()
            janela.append(float(perda)); passo_f = p + 1
            if passo_f % avaliar_cada == 0 or passo_f == passos:
                m = metricas(modelo, dev)
                reg = {"passo": passo_f, "perda_treino": round(sum(janela) / len(janela), 4), "perda_dev": round(m["perda"], 4),
                       "top1_dev": round(m["top1"], 4), "recall3_dev": round(m["recall3"], 4), "t": round(time.time() - t0, 1), "rss_mb": round(rss_mb())}
                janela = []; hist.append(reg); arq.write(json.dumps(reg) + "\n"); arq.flush(); log(json.dumps(reg))
                # seleção por ACURÁCIA de dev (top-1; desempate: menor perda). A perda de dev engana: a rede fica confiante nos erros.
                if m["top1"] > melhor[0]:
                    melhor, ruim = (m["top1"], m["perda"]), 0
                    C.salvar(melhor_p, modelo, semente=semente, hash_dataset=man["hash_conjunto"], passo=passo_f, historico=hist,
                             extras={"top1_dev": m["top1"], "perda_dev": m["perda"]})
                else:
                    ruim += 1
                    if ruim >= paciencia:
                        motivo = "parada_antecipada_dev"; break
                C.salvar(ultimo, modelo, semente=semente, hash_dataset=man["hash_conjunto"], passo=passo_f, historico=hist, otimizador=opt.state_dict(),
                         extras={"intermediario": True})
            if max_memoria_mb and rss_mb() > max_memoria_mb:
                motivo = "memoria"; break
            if max_segundos and time.time() - t0 >= max_segundos:
                motivo = "tempo"; break
    # ultimo.pt = estado REAL do fim do treino (retomável); melhor.pt = melhor acurácia de dev (o que se avalia/usa). Nada é sobrescrito.
    meta = C.salvar(ultimo, modelo, semente=semente, hash_dataset=man["hash_conjunto"], passo=passo_f, historico=hist, otimizador=opt.state_dict(),
                    extras={"motivo_parada": motivo, "melhor_top1_dev": melhor[0]})
    return {"checkpoint": ultimo, "checkpoint_melhor": melhor_p, "passo_final": passo_f, "motivo_parada": motivo, "parametros": modelo.n_parametros(), "dev": metricas(modelo, dev),
            "segundos": round(time.time() - t0, 1), "memoria_max_mb": round(rss_mb()), "meta": meta}
