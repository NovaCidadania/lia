"""CLI local do modelo próprio. Sempre offline.

  lia-neural dados    --saida DIR [--semente 1 --n-treino 6000 --n-dev 300 --n-teste 300]
  lia-neural treinar  --dados DIR --saida DIR [--passos N --max-segundos S --max-memoria-mb M --max-exemplos K --retomar CKPT ...]
  lia-neural avaliar  --dados DIR --checkpoint CKPT [--particao teste|dev] [--n N] --saida REL.json
  lia-neural propor   --checkpoint CKPT --pedido "..." --arquivo m.py
  lia-neural propor-lia --checkpoint CKPT            (prompt de coding da Lia no stdin -> JSON de edições no stdout)
  lia-neural e2e-lia  --dados DIR --checkpoint CKPT [--n 10] --saida REL.json   (lia-sessiond real, backend lia_proprio)
  lia-neural reparo-dados | reparo-treinar | reparo-avaliar     (V2: localizador neural de defeitos; mesmos argumentos, ver README)
  lia-neural info     --checkpoint CKPT"""
import argparse, hashlib, json, os, resource, sys, time

from . import dados as D


def _grava_dados(saida, ds, man):
    os.makedirs(saida, exist_ok=True)
    for k, v in ds.items():
        with open(os.path.join(saida, k + ".jsonl"), "w") as f:
            for e in v:
                f.write(json.dumps(e, ensure_ascii=False, sort_keys=True) + "\n")
    with open(os.path.join(saida, "manifesto.json"), "w") as f:
        json.dump(man, f, ensure_ascii=False, indent=1)


def carregar_dados(d):
    """lê as partições e CONFERE os hashes do manifesto (dataset adulterado ou de outra versão é recusado)"""
    man = json.load(open(os.path.join(d, "manifesto.json")))
    ds = {k: [json.loads(l) for l in open(os.path.join(d, k + ".jsonl"))] for k in ("treino", "dev", "teste")}
    for k, v in ds.items():
        if D.hash_exemplos(v) != man["particoes"][k]["hash"]:
            raise SystemExit("dataset %s não confere com o manifesto (hash)" % k)
    return ds, man


def carregar_dados_reparo(d):
    from . import repair_dados as RD
    man = json.load(open(os.path.join(d, "manifesto.json")))
    ds = {k: [json.loads(l) for l in open(os.path.join(d, k + ".jsonl"))] for k in ("treino", "dev", "teste")}
    for k, v in ds.items():
        if RD.hash_exemplos(v) != man["particoes"][k]["hash"]:
            raise SystemExit("dataset %s não confere com o manifesto (hash)" % k)
    return ds, man


def main(argv=None):
    ap = argparse.ArgumentParser(prog="lia-neural", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("dados"); s.add_argument("--saida", required=True)
    s.add_argument("--semente", type=int, default=1); s.add_argument("--n-treino", type=int, default=6000)
    s.add_argument("--n-dev", type=int, default=300); s.add_argument("--n-teste", type=int, default=300)
    t = sub.add_parser("treinar"); t.add_argument("--dados", required=True); t.add_argument("--saida", required=True)
    t.add_argument("--semente", type=int, default=1); t.add_argument("--passos", type=int, default=3000)
    t.add_argument("--lote", type=int, default=24); t.add_argument("--lr", type=float, default=2e-3)
    t.add_argument("--max-segundos", type=float); t.add_argument("--max-memoria-mb", type=int); t.add_argument("--max-exemplos", type=int)
    t.add_argument("--retomar"); t.add_argument("--threads", type=int)
    t.add_argument("--d", type=int); t.add_argument("--camadas", type=int); t.add_argument("--cabecas", type=int)
    t.add_argument("--salvar-cada", type=int, default=500); t.add_argument("--avaliar-cada", type=int, default=250)
    a = sub.add_parser("avaliar"); a.add_argument("--dados", required=True); a.add_argument("--checkpoint", required=True)
    a.add_argument("--particao", default="teste", choices=["dev", "teste"]); a.add_argument("--n", type=int); a.add_argument("--saida", required=True)
    a.add_argument("--threads", type=int)
    p = sub.add_parser("propor"); p.add_argument("--checkpoint", required=True); p.add_argument("--pedido", required=True); p.add_argument("--arquivo", required=True)
    q = sub.add_parser("propor-lia"); q.add_argument("--checkpoint", required=True)
    e = sub.add_parser("e2e-lia"); e.add_argument("--dados", required=True); e.add_argument("--checkpoint", required=True)
    e.add_argument("--particao", default="teste", choices=["dev", "teste"]); e.add_argument("--n", type=int, default=10); e.add_argument("--saida", required=True)
    rd = sub.add_parser("reparo-dados"); rd.add_argument("--saida", required=True); rd.add_argument("--semente", type=int, default=1)
    rd.add_argument("--n-treino", type=int, default=8000); rd.add_argument("--n-dev", type=int, default=400); rd.add_argument("--n-teste", type=int, default=900)
    rt = sub.add_parser("reparo-treinar"); rt.add_argument("--dados", required=True); rt.add_argument("--saida", required=True)
    rt.add_argument("--semente", type=int, default=1); rt.add_argument("--passos", type=int, default=4000); rt.add_argument("--lote", type=int, default=16)
    rt.add_argument("--lr", type=float, default=1e-3); rt.add_argument("--max-segundos", type=float); rt.add_argument("--max-memoria-mb", type=int)
    rt.add_argument("--max-exemplos", type=int); rt.add_argument("--retomar"); rt.add_argument("--threads", type=int)
    rt.add_argument("--d", type=int); rt.add_argument("--camadas", type=int); rt.add_argument("--cabecas", type=int); rt.add_argument("--dropout", type=float)
    rt.add_argument("--avaliar-cada", type=int, default=250); rt.add_argument("--paciencia", type=int, default=8)
    ra = sub.add_parser("reparo-avaliar"); ra.add_argument("--dados", required=True); ra.add_argument("--checkpoint", required=True)
    ra.add_argument("--particao", default="dev", choices=["dev", "teste"]); ra.add_argument("--n", type=int); ra.add_argument("--saida", required=True)
    ra.add_argument("--threads", type=int)
    i = sub.add_parser("info"); i.add_argument("--checkpoint", required=True)
    r = ap.parse_args(argv)

    if r.cmd == "dados":
        ds, man = D.gerar(r.semente, r.n_treino, r.n_dev, r.n_teste)
        erros = D.sem_vazamento(ds)
        if erros:
            raise SystemExit("vazamento: " + "; ".join(erros))
        _grava_dados(r.saida, ds, man)
        print(json.dumps(man["particoes"], ensure_ascii=False)); print("hash_conjunto", man["hash_conjunto"])
        return 0
    if r.cmd == "reparo-dados":
        from . import repair_dados as RD
        ds, man = RD.gerar(r.semente, r.n_treino, r.n_dev, r.n_teste)
        erros = RD.sem_vazamento(ds)
        if erros:
            raise SystemExit("vazamento: " + "; ".join(erros))
        _grava_dados(r.saida, ds, man)
        print(json.dumps({k: {"n": v["n"], "hash": v["hash"]} for k, v in man["particoes"].items()})); print("hash_conjunto", man["hash_conjunto"])
        return 0
    import torch  # só depois: 'dados' não precisa de torch
    if getattr(r, "threads", None):
        torch.set_num_threads(r.threads)
    from .checkpoint import CheckpointInvalido, carregar
    if r.cmd == "treinar":
        from .treino import treinar
        ds, man = carregar_dados(r.dados)
        cfg = {k: v for k, v in (("d", r.d), ("camadas", r.camadas), ("cabecas", r.cabecas)) if v}
        res = treinar(ds, man, r.saida, config=cfg, semente=r.semente, passos=r.passos, lote_tam=r.lote, lr=r.lr, max_segundos=r.max_segundos,
                      max_memoria_mb=r.max_memoria_mb, max_exemplos=r.max_exemplos, retomar=r.retomar, threads=r.threads,
                      salvar_cada=r.salvar_cada, avaliar_cada=r.avaliar_cada)
        print(json.dumps(res, ensure_ascii=False, default=str, indent=1))
        return 0
    try:
        if r.cmd == "reparo-treinar":
            from .repair_treino import treinar
            ds, man = carregar_dados_reparo(r.dados)
            cfg = {k: v for k, v in (("d", r.d), ("camadas", r.camadas), ("cabecas", r.cabecas), ("dropout", r.dropout)) if v is not None}
            res = treinar(ds, man, r.saida, config=cfg, semente=r.semente, passos=r.passos, lote_tam=r.lote, lr=r.lr, max_segundos=r.max_segundos,
                          max_memoria_mb=r.max_memoria_mb, max_exemplos=r.max_exemplos, retomar=r.retomar, threads=r.threads,
                          avaliar_cada=r.avaliar_cada, paciencia=r.paciencia)
            print(json.dumps(res, ensure_ascii=False, default=str, indent=1))
            return 0
        if r.cmd == "reparo-avaliar":
            from . import repair_avaliacao as RA, repair_checkpoint as RC
            ds, man = carregar_dados_reparo(r.dados)
            ex = ds[r.particao][:r.n] if r.n else ds[r.particao]
            t0 = time.time(); modelo, d = RC.carregar(r.checkpoint)
            rel = RA.avaliar_condicoes(ex, modelo)
            meta = {"particao": r.particao, "n_exemplos": len(ex), "familias": sorted({e["familia"] for e in ex}), "hash_particao": man["particoes"][r.particao]["hash"],
                    "hash_conjunto": man["hash_conjunto"], "gerador": man["gerador"], "semente_dados": man["semente"], "checkpoint": os.path.abspath(r.checkpoint),
                    "checkpoint_sha256": hashlib.sha256(open(r.checkpoint, "rb").read()).hexdigest(), "checkpoint_bytes": os.path.getsize(r.checkpoint),
                    "parametros": modelo.n_parametros(), "passo": d["passo"], "semente_treino": d["semente"], "segundos_total": round(time.time() - t0, 1),
                    "memoria_max_mb": round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024)}
            v = RA.veredito(rel)
            with open(r.saida, "w") as f:
                json.dump({"meta": meta, "veredito": v, "condicoes": rel}, f, ensure_ascii=False, indent=1)
            print(json.dumps({"meta": {k: meta[k] for k in ("particao", "n_exemplos", "checkpoint_sha256", "parametros", "passo", "segundos_total", "memoria_max_mb")},
                              "veredito": v}, ensure_ascii=False, indent=1))
            return 0
        if r.cmd == "avaliar":
            from .avaliacao import avaliar, veredito
            ds, man = carregar_dados(r.dados)
            ex = ds[r.particao][:r.n] if r.n else ds[r.particao]
            t0 = time.time()
            rel, modelo, d = avaliar(ex, r.checkpoint)
            meta = {"particao": r.particao, "n_exemplos": len(ex), "hash_particao": man["particoes"][r.particao]["hash"], "hash_conjunto": man["hash_conjunto"],
                    "checkpoint": os.path.abspath(r.checkpoint), "checkpoint_sha256": hashlib.sha256(open(r.checkpoint, "rb").read()).hexdigest(),
                    "checkpoint_bytes": os.path.getsize(r.checkpoint), "parametros": modelo.n_parametros(), "passo": d["passo"], "semente_treino": d["semente"],
                    "segundos_total": round(time.time() - t0, 1), "memoria_max_mb": round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024)}
            v = veredito(rel)
            with open(r.saida, "w") as f:
                json.dump({"meta": meta, "veredito": v, "condicoes": rel}, f, ensure_ascii=False, indent=1)
            print(json.dumps({"meta": meta, "veredito": v}, ensure_ascii=False, indent=1))
            return 0
        if r.cmd == "e2e-lia":
            from .e2e_lia import rodar
            ds, man = carregar_dados(r.dados)
            ex = [x for x in ds[r.particao] if x.get("tipo", "editar") == "editar"][:r.n]
            rel = rodar(ex, r.checkpoint)
            rel["meta"] = {"hash_particao": man["particoes"][r.particao]["hash"], "checkpoint_sha256": hashlib.sha256(open(r.checkpoint, "rb").read()).hexdigest()}
            with open(r.saida, "w") as f:
                json.dump(rel, f, ensure_ascii=False, indent=1)
            print(json.dumps({k: rel[k] for k in ("n", "classes", "falsos_concluido")}))
            return 0
        if r.cmd == "propor":
            from .inferencia import propor
            modelo, d = carregar(r.checkpoint)
            res = propor(modelo, r.pedido, open(r.arquivo).read())
            print(json.dumps({"proposta": res["proposta"], "tokens": res["tokens"]}, ensure_ascii=False))
            return 0
        if r.cmd == "propor-lia":
            from . import repair_checkpoint as RC
            if RC.formato_de(r.checkpoint) == "repair":
                from .repair_inferencia import propor_para_lia
                print(json.dumps(propor_para_lia(r.checkpoint, sys.stdin.read()), ensure_ascii=False))
                return 0
            from .inferencia import propor_para_lia
            print(json.dumps(propor_para_lia(r.checkpoint, sys.stdin.read()), ensure_ascii=False))
            return 0
        if r.cmd == "info":
            from . import repair_checkpoint as RC
            if RC.formato_de(r.checkpoint) == "repair":
                carregar = RC.carregar
            m, d = carregar(r.checkpoint)
            print(json.dumps({"sha256": hashlib.sha256(open(r.checkpoint, "rb").read()).hexdigest(), "bytes": os.path.getsize(r.checkpoint),
                              "parametros": m.n_parametros(), "config": m.config, "passo": d["passo"], "semente": d["semente"],
                              "hash_dataset": d["hash_dataset"], "versao_codigo": d["versao_codigo"], "tokenizer": d["tokenizer"]}, indent=1))
            return 0
    except CheckpointInvalido as e:
        print("lia-neural: checkpoint inválido: %s" % e, file=sys.stderr)
        return 3
    return 2


if __name__ == "__main__":
    sys.exit(main())
