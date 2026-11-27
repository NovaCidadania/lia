#!/usr/bin/env python3
"""Testes do localizador V2 (candidatos, dataset, no-leak, determinismo, checkpoint, retomada, inferência, formato inválido, adaptador, CLI).
Roda com: modelo-proprio/.venv/bin/python testes/modelo_proprio/teste_reparo.py"""
import json, os, shutil, subprocess, sys, tempfile, warnings
warnings.filterwarnings("ignore")
RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(RAIZ, "modelo-proprio"))
import torch  # noqa: E402
from lia_neural import repair_dados as RD, repair_modelo as M, repair_checkpoint as C, repair_treino as T, repair_avaliacao as A, repair_inferencia as I  # noqa: E402
from lia_neural.checkpoint import CheckpointInvalido  # noqa: E402
from lia_neural.bpe import BPE, pretokens  # noqa: E402

falhas = total = 0


def checar(cond, msg, extra=None):
    global falhas, total
    total += 1
    print(("ok   " if cond else "FALHA ") + msg + ("" if cond or extra is None else "\n      %s" % (extra,)))
    if not cond:
        falhas += 1


TINY = dict(d=32, camadas=2, cabecas=2)
_c = {}


def ds():
    if "x" not in _c:
        _c["x"] = RD.gerar(3, 60, 20, 20)
    return _c["x"]


def teste_candidatos():
    src = "def f(a, b):\n    if a > b:\n        return a - 1\n    return b\n"
    cs = RD.candidatos(src)
    pares = {(c["antigo"], c["novo"]) for c in cs}
    checar(("<", ">") not in pares and (">", "<") in pares and ("-", "+") in pares and ("1", "0") in pares and ("1", "2") in pares, "candidatas: operador, comparador e constante ±1")
    c = [x for x in cs if x["antigo"] == ">" and x["novo"] == "<"][0]
    novo = RD.aplicar(src, c)
    checar(novo == src.replace("a > b", "a < b") and src[c["c0"]:c["c1"]] == ">" and src.encode()[c["ini"]:c["fim"]] == b">", "aplicar troca só o token; offsets de caractere e byte conferem")
    checar(c["procurar"] == "    if a > b:" and c["substituir"] == "    if a < b:" and src.count(c["procurar"]) == 1, "patch = linha exata e única")
    dup = "def f(a):\n    x = a + 1\n    x = a + 1\n    return x\n"
    checar(all(c["procurar"] != "    x = a + 1" for c in RD.candidatos(dup)), "linha duplicada no arquivo não vira candidata (patch ambíguo)")
    checar(RD.candidatos("def f(:\n") == [] and RD.candidatos("x = 'a > b'\n") == [], "código inválido ou só texto: sem candidatas")
    todas = {t for g in RD.GRUPOS for t in g}
    checar(all(RD.TROCAS[n] and all(a in RD.TROCAS[n] for a in [o for o in todas if n in RD.TROCAS[o]]) for n in todas), "tabela de trocas é simétrica (todo defeito tem troca inversa)")


def teste_dados():
    d, man = ds()
    ex = [A._saidas(e["correto"], e, e["visivel"] + e["oculto"]) for e in d["teste"][:10]]
    rep = [A._saidas(RD.aplicar(e["codigo"], e["candidatos"][e["alvo"]]), e, e["visivel"] + e["oculto"]) for e in d["teste"][:10]]
    checar(ex == rep, "aplicar a troca inversa reproduz o comportamento do código correto (oráculo)")
    checar(all(A._saidas(e["codigo"], e, e["visivel"][:1]) != A._saidas(e["correto"], e, e["visivel"][:1]) for e in d["teste"]), "o primeiro caso visível expõe o defeito")
    checar(not RD.sem_vazamento(d), "sem vazamento: famílias disjuntas e nenhum (pedido, código) repetido", RD.sem_vazamento(d))
    f = RD.familias_particionadas()
    checar(not (set(f["treino"]) & set(f["teste"])) and not (set(f["dev"]) & set(f["teste"])) and len(f["teste"]) == 18, "partição por família: teste = 18 famílias nunca vistas")
    d2, man2 = RD.gerar(3, 60, 20, 20)
    checar(man["hash_conjunto"] == man2["hash_conjunto"] and all(man["particoes"][k]["hash"] == man2["particoes"][k]["hash"] for k in d), "geração é determinística (mesmos hashes)")
    checar(set(man) >= {"semente", "gerador", "familias", "licenca", "particoes", "hash_conjunto"} and all(man["particoes"][k]["n"] == len(d[k]) for k in d), "manifesto: semente, versão, famílias, licença, hash e contagem por partição")
    inf = "def f(n):\n    while True:\n        n += 1\n"
    checar(RD._ex(inf, "f", (1,)) == ("erro", "Tempo"), "laço infinito cai por limite de linhas (determinístico, sem relógio)")


def teste_modelo_e_checkpoint():
    d, man = ds()
    torch.manual_seed(1)
    m = M.Localizador(TINY)
    e = d["dev"][0]
    ordem = A.ordem_modelo(m, e)
    checar(sorted(ordem) == list(range(len(e["candidatos"]))), "inferência ordena todas as candidatas")
    with tempfile.TemporaryDirectory() as t:
        p = os.path.join(t, "c.pt")
        meta = C.salvar(p, m, semente=1, hash_dataset=man["hash_conjunto"], passo=5, historico=[])
        m2, dd = C.carregar(p)
        checar(dd["semente"] == 1 and dd["hash_dataset"] == man["hash_conjunto"] and dd["versao_codigo"] == M.VERSAO and dd["config"]["d"] == 32 and meta["parametros"] == m.n_parametros(),
               "checkpoint traz pesos, config, versão do código, semente e hash do dataset")
        checar(A.ordem_modelo(m2, e) == ordem and A.pontuar(m, e) == A.pontuar(m2, e), "salvar → recarregar → mesmas pontuações")
        for nome, f in (("ausente", lambda: C.carregar(os.path.join(t, "nao.pt"))),):
            try:
                f(); ok = False
            except CheckpointInvalido:
                ok = True
            checar(ok, "checkpoint %s falha com CheckpointInvalido" % nome)
        with open(p, "ab") as f:
            f.write(b"x")
        try:
            C.carregar(p); ok = False
        except CheckpointInvalido as ex:
            ok = "sha256" in str(ex)
        checar(ok, "checkpoint adulterado é recusado (sha256)")
        p2 = os.path.join(t, "v1.pt")
        torch.save({"formato": "outro"}, p2)
        try:
            C.carregar(p2); ok = False
        except CheckpointInvalido:
            ok = True
        checar(ok and C.formato_de(p2) == "v1", "formato desconhecido é recusado; despacho distingue V1 de V2")


def teste_treino_e_retomada():
    d, man = ds()
    with tempfile.TemporaryDirectory() as t:
        a = T.treinar(d, man, os.path.join(t, "a"), config=TINY, passos=12, lote_tam=4, avaliar_cada=6, paciencia=99, log=lambda *_: None)
        checar(a["parametros"] > 0 and a["passo_final"] > 0 and os.path.exists(os.path.join(t, "a", "treino.jsonl")), "treino roda, registra log e checkpoint")
        b = T.treinar(d, man, os.path.join(t, "b"), config=TINY, passos=6, lote_tam=4, avaliar_cada=6, paciencia=99, log=lambda *_: None)
        import lia_neural.repair_checkpoint as RC
        mb, db = RC.carregar(os.path.join(t, "b", "ultimo.pt"))
        checar(db["passo"] <= 6 and db["hash_dataset"] == man["hash_conjunto"], "checkpoint intermediário guarda passo e hash")
        r = T.treinar(d, man, os.path.join(t, "r"), config=TINY, passos=12, lote_tam=4, avaliar_cada=6, paciencia=99, retomar=os.path.join(t, "b", "ultimo.pt"), log=lambda *_: None)
        checar(r["passo_final"] >= 6, "retomar continua do passo salvo")
        d_outro, man_outro = RD.gerar(4, 20, 10, 10)
        try:
            T.treinar(d_outro, man_outro, os.path.join(t, "x"), config=TINY, passos=2, retomar=os.path.join(t, "b", "ultimo.pt"), log=lambda *_: None); ok = False
        except RuntimeError as ex:
            ok = "outro dataset" in str(ex)
        checar(ok, "retomar com dataset diferente é recusado")
        s = T.treinar(d, man, os.path.join(t, "s"), config=TINY, passos=1000, lote_tam=4, max_segundos=1, avaliar_cada=1000, log=lambda *_: None)
        checar(s["motivo_parada"] == "tempo", "limite de tempo interrompe o treino")
        s = T.treinar(d, man, os.path.join(t, "e"), config=TINY, passos=3, lote_tam=2, max_exemplos=5, avaliar_cada=3, log=lambda *_: None)
        checar(s["passo_final"] <= 3, "limite de exemplos respeitado")
    # perda cai de verdade ao treinar num conjunto minúsculo (sem simulação)
    with tempfile.TemporaryDirectory() as t:
        pq = {"treino": d["treino"][:8], "dev": d["dev"][:8], "teste": d["teste"][:4]}
        r = T.treinar(pq, man, os.path.join(t, "o"), config=TINY, passos=150, lote_tam=8, lr=3e-3, avaliar_cada=50, paciencia=99, log=lambda *_: None)
        linhas = [json.loads(l) for l in open(os.path.join(t, "o", "treino.jsonl"))]
        checar(linhas[-1]["perda_treino"] < linhas[0]["perda_treino"], "perda de treino cai (%.3f → %.3f)" % (linhas[0]["perda_treino"], linhas[-1]["perda_treino"]))


def teste_inferencia_e_adaptador():
    d, man = ds()
    torch.manual_seed(2)
    m = M.Localizador(TINY)
    e = d["dev"][0]
    p = I.propor(m, e["pedido"], e["codigo"], e["funcao"], k=3)
    checar(len(p) >= 1 and all(e["codigo"].count(x["procurar"]) == 1 for x in p), "propor devolve edições aplicáveis (procurar único) só dentro da função")
    checar(all(k in p[0] for k in ("procurar", "substituir")) and "concluído" not in json.dumps(p, ensure_ascii=False), "a saída não tem campo nem texto de conclusão")
    checar(I.propor(m, "x", "def g():\n    return 1\n", "inexistente") == [], "função inexistente: nenhuma edição")
    with tempfile.TemporaryDirectory() as t:
        ck = os.path.join(t, "c.pt"); C.salvar(ck, m, semente=1, hash_dataset=man["hash_conjunto"], passo=1, historico=[])
        prompt = "x\n----- pedido -----\n%s\n----- critérios de aceite -----\n- ok\n----- arquivos lidos -----\n=== m.py ===\n%s\n----- fim -----\n" % (e["pedido"], e["codigo"])
        r = I.propor_para_lia(ck, prompt)
        checar(r["motivo"] == "editar" and r["edicoes"][0]["acao"] == "editar" and r["edicoes"][0]["caminho"] == "m.py" and r["modelo"]["checkpoint_sha256"], "adaptador: contrato de edição da Lia + identidade do checkpoint")
        r = I.propor_para_lia(ck, prompt.replace(e["funcao"], "zzz_outra").replace("def zzz_outra", "def " + e["funcao"]).replace(e["pedido"], "Conserte o código, está errado."))
        checar(r["motivo"] == "esclarecer" and r["edicoes"] == [], "função não citada: pede esclarecimento (regra determinística), sem edição")
        r = I.propor_para_lia(ck, "texto qualquer")
        checar(r["motivo"] == "fora_do_escopo" and r["edicoes"] == [], "prompt que não é pedido de edição: fora do escopo")
        try:
            I.propor_para_lia(os.path.join(t, "nao.pt"), prompt); ok = False
        except CheckpointInvalido:
            ok = True
        checar(ok, "checkpoint ausente falha claramente (sem recair em outro modelo)")


def teste_avaliacao():
    d, man = ds()
    torch.manual_seed(3)
    m = M.Localizador(TINY)
    rel = A.avaliar_condicoes(d["dev"][:8], m, log=lambda *_: None)
    checar(set(rel) == {"aleatorio", "treinado", "fonte_b0"} and all("top1" in v and "laco_k5" in v for v in rel.values()), "três condições, com métrica top-1 e laço do runtime")
    v = A.veredito(rel)
    checar(v["capacidade_declaravel_na_tarefa_estreita"] in (True, False) and "ic95_vs_b0_pp" in v, "veredito segue o critério pré-registrado (margem ≥ 10 pp e falsos = 0)")
    e = d["dev"][0]
    esp = A._saidas(e["correto"], e, e["visivel"] + e["oculto"])
    j = A.julgar(e, e["alvo"], esp)
    checar(j["oculto_ok"] and j["concluido"] and not j["falso_concluido"], "julgar: a troca inversa passa visível e oculto, sem falso concluído")


def teste_bpe():
    d, man = ds()
    corpus = [e["pedido"] + "\n" + e["codigo"] for e in d["treino"]]
    b = BPE.treinar(corpus, 300)
    checar(0 < len(b.merges) <= 300 and b.n_vocab == 256 + len(b.merges), "BPE treina só no corpus dado e tem vocabulário = 256 bytes + merges")
    txt = "def f(a, b):\n    return a >= b  # ação ✓ \u00e9\n"
    ids, offs = b.codificar(txt)
    def expandir(t):
        return bytes([t]) if t < 256 else expandir(b.merges[t - 256][0]) + expandir(b.merges[t - 256][1])
    checar(b"".join(expandir(t) for t in ids) == txt.encode() and len(ids) == len(offs), "round-trip exato (inclui acentos e símbolos: byte fallback)")
    checar(len(b.codificar("zzqqxxyy ✓ 🙂")[0]) >= len("zzqqxxyy ✓ 🙂".encode()) // 4 and b.codificar("🙂")[0] == list("🙂".encode()), "texto nunca visto cai em bytes (sem token desconhecido)")
    checar(b.identidade() == BPE(b.merges).identidade() and b.identidade() != BPE(b.merges[:-1]).identidade(), "identidade do tokenizador depende dos merges")
    checar([w for w, _ in pretokens("a>=b+=1//2")] == ["a", ">=", "b", "+=", "1", "//", "2"], "operadores compostos e números são pré-tokens inteiros")
    e = d["dev"][0]
    c = M.codificar(e["pedido"], e["codigo"], e["candidatos"], 320, b)
    ids2, pos, toks = c
    ok = True
    for (i, j), cand in zip(pos, e["candidatos"]):
        ok = ok and 0 <= i <= j < len(ids2) and b"".join(expandir(t) for t in ids2[i:j + 1]) == e["codigo"][cand["c0"]:cand["c1"]].encode()
    checar(ok, "cada candidata aponta exatamente para os tokens do trecho que a troca altera")
    checar(M.codificar(e["pedido"], e["codigo"], e["candidatos"], 10, b) is None, "não cabe no contexto: devolve None (nunca trunca em silêncio)")
    with tempfile.TemporaryDirectory() as t:
        r = T.treinar(d, man, os.path.join(t, "o"), config=dict(d=32, camadas=2, cabecas=2), passos=6, lote_tam=4, avaliar_cada=3, paciencia=99, tokenizador="bpe", n_merges=200, log=lambda *_: None)
        m, dd = C.carregar(r["checkpoint"])
        checar(m.bpe is not None and m.bpe.merges and dd["tokenizer"] == m.bpe.identidade() and os.path.exists(r["checkpoint_melhor"]), "checkpoint BPE carrega com o tokenizador embutido; melhor.pt separado de ultimo.pt")
        p1 = A.pontuar(m, e)
        m2, _ = C.carregar(r["checkpoint"])
        checar(p1 == A.pontuar(m2, e) and len(p1) == len(e["candidatos"]), "salvar → recarregar → mesmas pontuações (BPE)")
        ult = torch.load(r["checkpoint"], map_location="cpu", weights_only=True)
        checar(ult["passo"] == 6 and ult["otimizador"] is not None, "ultimo.pt guarda o estado real do fim do treino (retomável), não o melhor")


def teste_cli():
    cli = os.path.join(RAIZ, "modelo-proprio", "lia-neural")
    with tempfile.TemporaryDirectory() as t:
        dd = os.path.join(t, "d")
        p = subprocess.run([cli, "reparo-dados", "--saida", dd, "--n-treino", "24", "--n-dev", "8", "--n-teste", "8"], capture_output=True, text=True)
        checar(p.returncode == 0 and os.path.exists(os.path.join(dd, "manifesto.json")), "CLI reparo-dados")
        ck = os.path.join(t, "ck")
        p = subprocess.run([cli, "reparo-treinar", "--dados", dd, "--saida", ck, "--passos", "6", "--lote", "4", "--d", "32", "--camadas", "2", "--cabecas", "2", "--avaliar-cada", "3"], capture_output=True, text=True)
        checar(p.returncode == 0 and os.path.exists(os.path.join(ck, "ultimo.pt")), "CLI reparo-treinar", p.stderr[-300:])
        rel = os.path.join(t, "r.json")
        p = subprocess.run([cli, "reparo-avaliar", "--dados", dd, "--checkpoint", os.path.join(ck, "ultimo.pt"), "--particao", "teste", "--saida", rel], capture_output=True, text=True)
        checar(p.returncode == 0 and "veredito" in json.load(open(rel)), "CLI reparo-avaliar", p.stderr[-300:])
        p = subprocess.run([cli, "info", "--checkpoint", os.path.join(ck, "ultimo.pt")], capture_output=True, text=True)
        checar(p.returncode == 0 and json.loads(p.stdout)["versao_codigo"] == M.VERSAO, "CLI info lê checkpoint V2")
        e = json.loads(open(os.path.join(dd, "dev.jsonl")).readline())
        prompt = "----- pedido -----\n%s\n----- critérios de aceite -----\n- ok\n----- arquivos lidos -----\n=== m.py ===\n%s\n" % (e["pedido"], e["codigo"])
        p = subprocess.run([cli, "propor-lia", "--checkpoint", os.path.join(ck, "ultimo.pt")], input=prompt, capture_output=True, text=True)
        checar(p.returncode == 0 and json.loads(p.stdout)["motivo"] == "editar", "CLI propor-lia usa o localizador V2", p.stderr[-300:])
        p = subprocess.run([cli, "propor-lia", "--checkpoint", os.path.join(t, "nao.pt")], input=prompt, capture_output=True, text=True)
        checar(p.returncode == 3, "CLI propor-lia com checkpoint ausente: rc 3, sem fallback")
        with open(os.path.join(dd, "teste.jsonl"), "a") as f:
            f.write(open(os.path.join(dd, "teste.jsonl")).readline())
        p = subprocess.run([cli, "reparo-avaliar", "--dados", dd, "--checkpoint", os.path.join(ck, "ultimo.pt"), "--saida", rel], capture_output=True, text=True)
        checar(p.returncode != 0 and "não confere" in (p.stderr + p.stdout), "dataset adulterado é recusado (hash do manifesto)")


if __name__ == "__main__":
    for f in (teste_candidatos, teste_dados, teste_modelo_e_checkpoint, teste_treino_e_retomada, teste_inferencia_e_adaptador, teste_avaliacao, teste_bpe, teste_cli):
        print("== " + f.__name__)
        f()
    print("\n%d testes, %d falhas" % (total, falhas))
    sys.exit(1 if falhas else 0)
