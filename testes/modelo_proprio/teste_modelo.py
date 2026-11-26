#!/usr/bin/env python3
"""Testes do modelo próprio (tokenizer, dataset, sandbox, runtime, checkpoint, treino/retomada, inferência, CLI).
Roda com o python do venv do projeto: modelo-proprio/.venv/bin/python testes/modelo_proprio/teste_modelo.py"""
import json, os, subprocess, sys, tempfile, shutil, warnings
warnings.filterwarnings("ignore")
RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(RAIZ, "modelo-proprio"))
import torch  # noqa: E402
from lia_neural import dados as D, tokenizer as tk, sandbox, runtime, baseline, inferencia  # noqa: E402
from lia_neural.checkpoint import carregar, salvar, CheckpointInvalido, sha256_arquivo  # noqa: E402
from lia_neural.modelo import LiaGPT  # noqa: E402
from lia_neural.treino import treinar, codificar  # noqa: E402

falhas = total = 0


def checar(cond, msg, extra=None):
    global falhas, total
    total += 1
    if cond:
        print("ok   " + msg)
    else:
        falhas += 1
        print("FALHA " + msg + ("\n      %s" % (extra,) if extra is not None else ""))


TINY = dict(d=32, camadas=2, cabecas=2, contexto=512)
_ds_cache = {}


def ds_pequeno():
    if "x" not in _ds_cache:
        _ds_cache["x"] = D.gerar(7, 120, 30, 30)
    return _ds_cache["x"]


def teste_tokenizer():
    p = tk.montar_prompt("Corrija f. [FIM] ✓", "def f():\n    pass\n")
    checar(p[0] == tk.BOS and p[-1] == tk.RESPOSTA and p.count(tk.PEDIDO) == 1 and p.count(tk.CODIGO) == 1, "prompt: BOS PEDIDO … CODIGO … RESPOSTA")
    checar(tk.FIM not in p and all(i < 256 for i in tk.texto_para_ids("[FIM] <|endoftext|> [EDITAR]")), "texto do usuário nunca vira marcador (\"[FIM]\" são só bytes)")
    checar(tk.ids_para_texto(tk.texto_para_ids("olá, mundo ✓ ç")) == "olá, mundo ✓ ç", "ida e volta UTF-8 (acentos, símbolos)")
    ed = tk.alvo_editar("    if a > b:", "    if a < b:")
    checar(tk.interpretar(ed) == {"tipo": "editar", "procurar": "    if a > b:", "substituir": "    if a < b:"}, "interpretar: edição válida")
    checar(tk.interpretar(tk.alvo_esclarecer("Qual arquivo?"))["tipo"] == "esclarecer" and tk.interpretar(tk.alvo_recusar("Não."))["tipo"] == "recusar", "interpretar: esclarecer e recusar")
    inval = {"sem FIM": ed[:-1], "texto depois do FIM é ignorado mas o bloco vale": None, "duas SUBSTITUIR": [tk.EDITAR, tk.PROCURAR, 65, tk.SUBSTITUIR, 66, tk.SUBSTITUIR, 67, tk.FIM],
             "procurar == substituir": tk.alvo_editar("x", "x"), "procurar vazio": tk.alvo_editar("", "x"), "marcador de entrada na saída": [tk.EDITAR, tk.PROCURAR, tk.CODIGO, tk.SUBSTITUIR, 66, tk.FIM],
             "começa sem tipo": [65, 66, tk.FIM], "vazio": [], "esclarecer vazio": [tk.ESCLARECER, tk.FIM], "recusar com marcador": [tk.RECUSAR, tk.PROCURAR, 65, tk.FIM]}
    for k, v in inval.items():
        if v is not None:
            checar(tk.interpretar(v) is None, "formato inválido rejeitado: " + k)
    checar(tk.interpretar(ed + tk.texto_para_ids("lixo depois")) == tk.interpretar(ed), "tokens depois do FIM são descartados")
    checar(tk.identidade() == tk.identidade() and len(tk.identidade()) == 64, "identidade do tokenizer estável (entra no checkpoint)")
    checar(tk.VOCAB == 268 and tk.FIM == 267, "vocabulário fixo: 256 bytes + 12 marcadores")


def teste_dados():
    ds, man = ds_pequeno()
    ds2, man2 = D.gerar(7, 120, 30, 30)
    checar(man["hash_conjunto"] == man2["hash_conjunto"] and ds == ds2, "gerador determinístico: mesma semente, mesmos bytes")
    ds3, man3 = D.gerar(8, 120, 30, 30)
    checar(man3["hash_conjunto"] != man["hash_conjunto"], "semente diferente muda o conjunto")
    checar({k: v["n"] for k, v in man["particoes"].items()} == {"treino": 120, "dev": 30, "teste": 30}, "contagens por partição no manifesto")
    checar(all(k in man for k in ("semente", "gerador", "codigo", "familias", "licenca", "hash_conjunto")), "manifesto guarda semente, versões, famílias, licença e hash")
    f = man["familias"]
    checar(not (set(f["treino"]) & set(f["dev"])) and not (set(f["treino"]) & set(f["teste"])) and not (set(f["dev"]) & set(f["teste"])), "famílias disjuntas entre treino/dev/teste")
    checar(len(f["treino"]) == 24 and len(f["dev"]) == 6 and len(f["teste"]) == 6, "24/6/6 famílias")
    checar(D.sem_vazamento(ds) == [], "sem vazamento: famílias dos exemplos disjuntas e nenhum (pedido, código) repetido entre partições")
    for k in ("treino", "dev", "teste"):
        fam_ex = {e["familia"] for e in ds[k] if e["tipo"] == "editar"}
        checar(fam_ex <= set(f[k]), "partição %s só usa famílias dela" % k)
        tipos = {e["tipo"] for e in ds[k]}
        checar(tipos == {"editar", "esclarecer", "recusar"}, "partição %s tem os três tipos de resposta (editar, esclarecer, recusar)" % k)
    # defeito realmente muda o comportamento, a edição-alvo aplica exatamente uma vez e produz o código correto
    ruins = 0
    for e in [x for x in ds["teste"] if x["tipo"] == "editar"][:12]:
        a = e["alvo"]
        ok_aplica = e["codigo"].count(a["procurar"]) == 1 and e["codigo"].replace(a["procurar"], a["substituir"], 1) == e["correto"]
        ent = e["visivel"] + e["oculto"]
        bom = sandbox.rodar_casos(e["correto"], e["funcao"], ent)
        ruim = sandbox.rodar_casos(e["codigo"], e["funcao"], ent)
        if not ok_aplica or bom == ruim or len(e["visivel"]) != 3 or len(e["oculto"]) != 12:
            ruins += 1
        if sandbox.rodar_casos(e["correto"], e["funcao"], e["visivel"])[0][0] != "ok":
            ruins += 1
    checar(ruins == 0, "cada exemplo de edição: defeito muda o comportamento, edição-alvo aplica 1x e dá o código correto, 3 visíveis + 12 ocultos", ruins)
    checar(all(e["alvo"].get("texto") for e in ds["teste"] if e["tipo"] != "editar") and all(not e["visivel"] for e in ds["teste"] if e["tipo"] != "editar"),
           "esclarecer/recusar: resposta é texto e não há suíte (o runtime nunca conclui nelas)")
    sint = [e for e in ds["treino"] if e["tipo"] == "editar"][0]
    checar("m.py" in sint["pedido"] and sint["funcao"] in sint["pedido"], "o pedido cita arquivo e função")
    antes = D.hash_exemplos(ds["teste"])
    checar(D.hash_exemplos(json.loads(json.dumps(ds["teste"]))) == antes, "hash da partição é estável à serialização")
    adulterado = json.loads(json.dumps(ds)); adulterado["teste"].append(dict(adulterado["treino"][0]))
    checar(D.sem_vazamento(adulterado) != [], "detector de vazamento pega exemplo copiado do treino para o teste")


def teste_sandbox_e_runtime():
    r = sandbox.rodar_casos("def f(a):\n    return a + 1\n", "f", [[1], ["x"]])
    checar(r == [("ok", 2), ("erro", "TypeError")], "sandbox: resultado e erro por caso", r)
    checar(sandbox.rodar_casos("def f(a):\n    while True: pass\n", "f", [[1]]) == [("erro", "TimeoutError")], "sandbox: laço infinito vira erro de tempo")
    checar(sandbox.rodar_casos("def f(a:\n", "f", [[1]]) == [("erro", "SyntaxError")], "sandbox: sintaxe inválida")
    net = sandbox.rodar_casos("import socket\ndef f(a):\n    socket.create_connection(('1.1.1.1', 80), 1)\n", "f", [[1]])
    checar(net[0][0] == "erro", "sandbox: sem rede", net)
    leit = sandbox.rodar_casos("def f(a):\n    return open('/home/zad/.bashrc').read()\n", "f", [[1]])
    checar(leit[0][0] == "erro", "sandbox: não enxerga o home", leit)
    sp = sandbox.rodar_casos("print('lixo')\ndef f(a):\n    return a\n", "f", [[3]])
    checar(sp == [("ok", 3)], "sandbox: print do código proposto não corrompe o resultado", sp)
    ds, _ = ds_pequeno()
    e = [x for x in ds["teste"] if x["tipo"] == "editar"][0]
    certa = runtime.julgar(e, {"tipo": "editar", **e["alvo"]})
    checar(certa["aplica"] and certa["visivel_ok"] and certa["oculto_ok"] and certa["concluido"] and not certa["falso_concluido"], "runtime: edição correta aplica, passa visível e oculto, conclui", certa)
    checar(runtime.julgar(e, {"tipo": "editar", "procurar": "inexistente_xyz", "substituir": "a"})["aplica"] is False, "runtime: trecho inexistente não aplica")
    amb = runtime.julgar(e, {"tipo": "editar", "procurar": "\n", "substituir": "\n\n"})
    checar(amb["aplica"] is False, "runtime: trecho ambíguo (várias ocorrências) não aplica")
    checar(runtime.julgar(e, None)["formato_valido"] is False, "runtime: proposta inválida")
    # falso "concluído": a edição passa a suíte visível mas erra o oráculo (casos especiais para as entradas visíveis)
    func = e["funcao"]
    vis = e["visivel"]
    esp = sandbox.rodar_casos(e["correto"], func, vis)
    tabela = {json.dumps(v): x[1] for v, x in zip(vis, esp)}
    trapaca = "def %s(*a):\n    t = %r\n    import json\n    return t.get(json.dumps(list(a)), 0)\n" % (func, tabela)
    e2 = dict(e); e2["codigo"] = e["codigo"]
    corpo_antigo = e["codigo"]
    r2 = runtime.julgar(e2, {"tipo": "editar", "procurar": corpo_antigo.strip("\n"), "substituir": trapaca.strip("\n")})
    checar(r2["concluido"] and not r2["oculto_ok"] and r2["falso_concluido"], "runtime: passa a suíte visível e reprova o oráculo => FALSO concluído (detectado)", r2)
    nc = [x for x in ds["teste"] if x["tipo"] != "editar"][0]
    rn = runtime.julgar(nc, {"tipo": "editar", "procurar": "x", "substituir": "y"})
    checar(rn["edicao_indevida"] and not rn["concluido"] and not rn["falso_concluido"], "runtime: edição onde devia pedir contexto/recusar é registrada e nunca conclui")
    checar(runtime.julgar(nc, {"tipo": nc["tipo"], "texto": "x"})["acao_correta"], "runtime: esclarecer/recusar certo conta como ação correta")
    m = runtime.agregar([certa, r2, rn])
    checar(m["falsos_concluido"] == 1 and m["concluidos"] == 2, "agregação conta falsos concluído e concluídos", m)
    checar(not any("conclu" in k for k in tk.NOMES), "o formato nativo do modelo não tem marcador de 'concluído'")
    # baseline
    cod = "def calcular(a, b):\n    if a < b:\n        return a\n    return b\n"
    b = baseline.b0("A função calcular devolve o menor", cod)
    checar(b == {"tipo": "editar", "procurar": "    if a < b:", "substituir": "    if a > b:"}, "baseline B0 determinístico: inverte o primeiro operador da função citada", b)
    checar(baseline.b0("corrija outra", cod)["tipo"] == "esclarecer", "baseline B0: função não citada => pede contexto")


def modelo_tiny(semente=3):
    torch.manual_seed(semente)
    return LiaGPT(TINY)


def teste_checkpoint_e_inferencia():
    with tempfile.TemporaryDirectory() as d:
        m = modelo_tiny()
        n = sum(p.numel() for p in m.parameters())
        checar(m.n_parametros() == n and n > 1000, "contagem de parâmetros", n)
        pa = os.path.join(d, "a.pt")
        meta = salvar(pa, m, semente=3, hash_dataset="h" * 64, passo=5, historico=[{"passo": 5}])
        checar(meta["sha256"] == sha256_arquivo(pa) and meta["bytes"] == os.path.getsize(pa) and meta["parametros"] == n, "checkpoint: meta com sha256, bytes e parâmetros")
        m2, c = carregar(pa)
        x = torch.tensor([tk.montar_prompt("p", "def f():\n    pass\n")])
        m.eval(); m2.eval()
        checar(torch.equal(m(x), m2(x)), "recarregar o checkpoint reproduz exatamente os mesmos logits")
        checar(c["semente"] == 3 and c["hash_dataset"] == "h" * 64 and c["versao_codigo"] and c["tokenizer"] == tk.identidade() and c["config"]["d"] == 32,
               "checkpoint guarda semente, hash do dataset, versão do código, tokenizer e configuração")
        g1 = m.gerar(x[0].tolist(), 20); g2 = m2.gerar(x[0].tolist(), 20)
        checar(g1 == g2, "inferência após recarregar = inferência antes de salvar")
        rp = inferencia.propor(m2, "p", "def f():\n    pass\n", max_novos=10)
        checar(rp["motivo"] == "ok" and rp["tokens"] <= 10, "propor roda offline e respeita o limite de tokens", rp["tokens"])
        # decodificação com cache == recomputo completo
        ids = list(x[0]); out = []
        with torch.no_grad():
            for _ in range(15):
                lg = m(torch.tensor([ids]))[0, -1].clone()
                lg[[tk.PAD, tk.BOS, tk.EOS, tk.PEDIDO, tk.CODIGO, tk.RESPOSTA]] = -1e9
                t = int(lg.argmax()); ids.append(t); out.append(t)
                if t == tk.FIM:
                    break
        checar(out == m.gerar(x[0].tolist(), 15), "decodificação incremental (cache) idêntica à recomputação completa")
        # corrompido, ausente, versão e tokenizer
        for nome, mut, esperado in (("ausente", None, "ausente"), ("bytes alterados", "corrompe", "sha256"), ("lixo", "lixo", "ilegível")):
            q = os.path.join(d, "t_%s.pt" % nome.replace(" ", "_"))
            if mut is None:
                q = os.path.join(d, "nao_existe.pt")
            else:
                shutil.copy(pa, q); shutil.copy(pa + ".json", q + ".json")
                with open(q, "r+b") as f:
                    f.seek(100) if mut == "corrompe" else None
                    f.write(b"\xff\xfe corrompido" if mut == "corrompe" else b"nao e um checkpoint")
                if mut == "lixo":
                    os.unlink(q + ".json")
            try:
                carregar(q); ok = False; msg = ""
            except CheckpointInvalido as e:
                ok, msg = esperado in str(e), str(e)
            checar(ok, "checkpoint %s: falha clara, sem recair em outro modelo" % nome, msg)
        raw = torch.load(pa, weights_only=True)
        for campo, valor, trecho in (("versao_codigo", "lia-neural/999", "outra versão"), ("tokenizer", "x" * 64, "outro tokenizer")):
            r = dict(raw); r[campo] = valor
            q = os.path.join(d, "v.pt"); torch.save(r, q)
            try:
                carregar(q); ok, msg = False, ""
            except CheckpointInvalido as e:
                ok, msg = trecho in str(e), str(e)
            checar(ok, "checkpoint com %s incompatível é recusado" % campo, msg)
        r = dict(raw); r["pesos"] = {k: v for k, v in raw["pesos"].items() if k != "tok.weight"}
        q = os.path.join(d, "p.pt"); torch.save(r, q)
        try:
            carregar(q); ok = False
        except CheckpointInvalido:
            ok = True
        checar(ok, "pesos faltando: recusado")
    # modelo aleatório gera formato inválido de forma estrita; nunca crasha
    m = modelo_tiny(11)
    inval = 0
    for i in range(5):
        r = inferencia.propor(m, "Corrija f em m.py", "def f(a):\n    return a\n", max_novos=40)
        inval += r["proposta"] is None
    checar(inval >= 4, "pesos aleatórios quase nunca produzem o formato estruturado (validação estrita)", inval)
    r = inferencia.propor(m, "x" * 600, "y" * 200)
    checar(r["proposta"] is None and r["motivo"] == "contexto_maior_que_o_modelo", "entrada maior que o contexto: recusada com motivo, não truncada em silêncio")


PROMPT_LIA = ("Você escreve código num projeto local. Proponha a MENOR mudança.\n\n----- pedido -----\nEm m.py, a função calcular deveria devolver o maior.\n"
              "\n----- critérios de aceite -----\n1. teste passa\n\n----- arquivos lidos -----\n=== m.py ===\ndef calcular(a, b):\n    if a < b:\n        return a\n    return b\n"
              "\n----- fim dos dados -----\n\nResponda SÓ com JSON\n")


def teste_adaptador_lia():
    ped, cam, cod = inferencia.extrair_do_prompt_lia(PROMPT_LIA)
    checar(ped.startswith("Em m.py") and cam == "m.py" and cod == "def calcular(a, b):\n    if a < b:\n        return a\n    return b\n", "extrai pedido, caminho e arquivo do prompt de coding da Lia", (ped, cam, cod))
    dois = PROMPT_LIA.replace("\n----- fim dos dados", "=== n.py ===\nx = 1\n\n----- fim dos dados")
    for texto, rot in (("sem pedido", "prompt que não é de coding"), (dois, "dois arquivos")):
        try:
            inferencia.extrair_do_prompt_lia(texto); ok = False
        except ValueError:
            ok = True
        checar(ok, "adaptador recusa: " + rot)
    with tempfile.TemporaryDirectory() as d:
        pa = os.path.join(d, "a.pt")
        salvar(pa, modelo_tiny(), semente=1, hash_dataset="h" * 64, passo=0, historico=[])
        r = inferencia.propor_para_lia(pa, PROMPT_LIA)
        checar(r["edicoes"] == [] or r["edicoes"][0]["acao"] == "editar", "propor_para_lia devolve o contrato de edições da Lia", r)
        checar(r["modelo"]["backend"] == "lia_proprio" and len(r["modelo"]["checkpoint_sha256"]) == 64, "resultado registra backend e sha256 do checkpoint", r["modelo"])
        checar(set(r) == {"edicoes", "explicacao", "modelo", "motivo"}, "a saída do adaptador não tem campo de conclusão", sorted(r))
        r2 = inferencia.propor_para_lia(pa, "isto não é um prompt de coding")
        checar(r2["edicoes"] == [] and r2["motivo"] == "fora_do_escopo", "prompt fora do escopo: nenhuma edição, motivo explícito")
        try:
            inferencia.propor_para_lia(os.path.join(d, "x.pt"), PROMPT_LIA); ok = False
        except CheckpointInvalido:
            ok = True
        checar(ok, "adaptador sem checkpoint: falha (não responde com outro modelo)")
        cli = os.path.join(RAIZ, "modelo-proprio", "lia-neural")
        p = subprocess.run([cli, "propor-lia", "--checkpoint", os.path.join(d, "x.pt")], input=PROMPT_LIA, capture_output=True, text=True)
        checar(p.returncode == 3 and "checkpoint inválido" in p.stderr and not p.stdout.strip(), "CLI com checkpoint ausente: código 3, erro claro, stdout vazio", (p.returncode, p.stderr[:200]))
        p = subprocess.run([cli, "propor-lia", "--checkpoint", pa], input=PROMPT_LIA, capture_output=True, text=True)
        j = json.loads(p.stdout) if p.returncode == 0 else {}
        checar(p.returncode == 0 and "edicoes" in j and j.get("modelo", {}).get("backend") == "lia_proprio", "CLI propor-lia com checkpoint válido: JSON de edições + identidade", p.stderr[:200])


def teste_treino_e_retomada():
    ds, man = ds_pequeno()
    with tempfile.TemporaryDirectory() as d:
        a = os.path.join(d, "a")
        r = treinar(ds, man, a, config=TINY, semente=5, passos=40, lote_tam=8, lr=3e-3, warmup=5, salvar_cada=20, avaliar_cada=20, threads=1, log=lambda *_: None)
        h = json.load(open(r["checkpoint"] + ".json"))
        checar(r["passo_final"] == 40 and os.path.getsize(r["checkpoint"]) > 10000 and h["sha256"] == sha256_arquivo(r["checkpoint"]), "treino real produz checkpoint com sha256 e tamanho")
        linhas = [json.loads(l) for l in open(os.path.join(a, "treino.jsonl"))]
        checar(len(linhas) >= 1 and linhas[-1]["perda_treino"] < r["perda_inicial"] - 0.5, "perda de treino decresce (de %.2f para %.2f)" % (r["perda_inicial"], linhas[-1]["perda_treino"]))
        checar(r["perda_dev_final"] is not None and r["perda_dev_final"] < r["perda_inicial"], "perda de validação medida e menor que a inicial", r["perda_dev_final"])
        m0 = modelo_tiny(5)   # mesma semente de inicialização
        m1, c = carregar(r["checkpoint"])
        dif = sum(float((p0 - p1).abs().sum()) for p0, p1 in zip(m0.parameters(), m1.parameters()))
        checar(dif > 1.0, "pesos treinados diferem dos pesos iniciais aleatórios (não é artefato constante)", dif)
        checar(len({round(float(p.std()), 6) for p in m1.parameters() if p.numel() > 100}) > 3, "pesos não são constantes")
        # retomada: 20 passos + retomar até 40 == 40 direto (mesma semente, mesmos lotes)
        b = os.path.join(d, "b")
        treinar(ds, man, b, config=TINY, semente=5, passos=40, lote_tam=8, lr=3e-3, warmup=5, salvar_cada=20, avaliar_cada=20, max_segundos=None, threads=1, log=lambda *_: None)
        # interrompe de verdade: 40 passos programados mas parado em 20 por limite de memória artificial
        e = os.path.join(d, "e")
        try:
            treinar(ds, man, e, config=TINY, semente=5, passos=40, lote_tam=8, lr=3e-3, warmup=5, salvar_cada=20, avaliar_cada=20, threads=1, log=lambda *_: None, max_memoria_mb=1)
            lev = False
        except RuntimeError as ex:
            lev = "limite de memória" in str(ex)
        checar(lev and os.path.exists(os.path.join(e, "ultimo.pt")), "limite de memória: para com erro claro e deixa checkpoint")
        _, ce = carregar(os.path.join(e, "ultimo.pt"))
        checar(ce["passo"] < 40, "o checkpoint da parada ficou antes do fim (passo %d)" % ce["passo"])
        r2 = treinar(ds, man, e, config=TINY, semente=5, passos=40, lote_tam=8, lr=3e-3, warmup=5, salvar_cada=20, avaliar_cada=20, threads=1, log=lambda *_: None, retomar=os.path.join(e, "ultimo.pt"))
        mb, _ = carregar(os.path.join(b, "ultimo.pt")); me, _ = carregar(os.path.join(e, "ultimo.pt"))
        igual = all(torch.allclose(x, y, atol=1e-5) for x, y in zip(mb.parameters(), me.parameters()))
        checar(r2["passo_final"] == 40 and igual, "retomar do checkpoint chega aos mesmos pesos do treino sem interrupção", r2["passo_final"])
        # outro dataset: recusado
        ds2, man2 = D.gerar(99, 60, 20, 20)
        try:
            treinar(ds2, man2, os.path.join(d, "f"), config=TINY, semente=5, passos=45, lote_tam=8, threads=1, log=lambda *_: None, retomar=os.path.join(e, "ultimo.pt")); ok = False
        except RuntimeError as ex:
            ok = "outro dataset" in str(ex)
        checar(ok, "retomar com dataset diferente é recusado")
        # limites de tempo e de exemplos
        g = os.path.join(d, "g")
        rg = treinar(ds, man, g, config=TINY, semente=5, passos=100000, lote_tam=8, threads=1, log=lambda *_: None, max_segundos=3, max_exemplos=40)
        checar(rg["motivo_parada"] == "tempo" and rg["passo_final"] < 100000, "limite de tempo para o treino (e salva checkpoint)", rg["passo_final"])
        x, _ = codificar(ds["treino"][0])
        checar(x[0] == tk.BOS and x[-1] == tk.FIM, "exemplo codificado: BOS … FIM")


def teste_cli_ponta_a_ponta():
    cli = os.path.join(RAIZ, "modelo-proprio", "lia-neural")
    with tempfile.TemporaryDirectory() as d:
        dd, ck, rel = os.path.join(d, "dados"), os.path.join(d, "ck"), os.path.join(d, "rel.json")
        p = subprocess.run([cli, "dados", "--saida", dd, "--semente", "3", "--n-treino", "80", "--n-dev", "24", "--n-teste", "24"], capture_output=True, text=True)
        checar(p.returncode == 0 and os.path.exists(os.path.join(dd, "manifesto.json")) and os.path.exists(os.path.join(dd, "teste.jsonl")), "CLI dados grava partições e manifesto", p.stderr[:200])
        p = subprocess.run([cli, "treinar", "--dados", dd, "--saida", ck, "--passos", "12", "--lote", "4", "--d", "32", "--camadas", "2", "--cabecas", "2", "--threads", "2",
                            "--avaliar-cada", "6", "--salvar-cada", "6"], capture_output=True, text=True)
        checar(p.returncode == 0 and os.path.exists(os.path.join(ck, "ultimo.pt")), "CLI treinar produz checkpoint", p.stderr[-300:])
        p = subprocess.run([cli, "avaliar", "--dados", dd, "--checkpoint", os.path.join(ck, "ultimo.pt"), "--n", "10", "--saida", rel, "--threads", "2"], capture_output=True, text=True)
        j = json.load(open(rel)) if os.path.exists(rel) else {}
        conds = set(j.get("condicoes", {}))
        checar(p.returncode == 0 and conds == {"aleatorio_sem_treino", "treinado_do_zero", "baseline_b0_primeira_inversao", "referencia_b1_busca_com_suite_visivel"},
               "CLI avaliar compara aleatório, treinado, B0 e B1", (p.stderr[-300:], conds))
        m = j.get("meta", {})
        checar(all(k in m for k in ("checkpoint_sha256", "checkpoint_bytes", "parametros", "memoria_max_mb", "segundos_total", "hash_particao")), "relatório registra sha, tamanho, parâmetros, memória e tempo")
        checar("capacidade_declaravel_na_tarefa_estreita" in j.get("veredito", {}), "relatório traz o veredito do critério fixado")
        # dataset adulterado é recusado
        with open(os.path.join(dd, "teste.jsonl"), "a") as f:
            f.write(open(os.path.join(dd, "teste.jsonl")).readline())
        p = subprocess.run([cli, "avaliar", "--dados", dd, "--checkpoint", os.path.join(ck, "ultimo.pt"), "--saida", rel], capture_output=True, text=True)
        checar(p.returncode != 0 and "não confere" in (p.stderr + p.stdout), "dataset adulterado é recusado (hash do manifesto)")


if __name__ == "__main__":
    for f in (teste_tokenizer, teste_dados, teste_sandbox_e_runtime, teste_checkpoint_e_inferencia, teste_adaptador_lia, teste_treino_e_retomada, teste_cli_ponta_a_ponta):
        print("== " + f.__name__)
        f()
    print("\n%d testes, %d falhas" % (total, falhas))
    sys.exit(1 if falhas else 0)
