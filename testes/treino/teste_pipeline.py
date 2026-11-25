#!/usr/bin/env python3
"""Pipeline de dados de treino (ferramentas/treino/lia_treino.py) com
trajetórias SINTÉTICAS: recusa sem licença/proveniência/validação,
rótulos (positivo só com validação depois do último patch), anonimização,
partição sem vazamento, exclusão do conjunto congelado, manifesto
reprodutível. Uso: make teste-treino"""
import copy, hashlib, json, os, shutil, sys, tempfile

AQUI = os.path.dirname(os.path.abspath(__file__))
LIA = os.path.abspath(os.path.join(AQUI, "..", ".."))
sys.path.insert(0, os.path.join(LIA, "ferramentas", "treino"))
import lia_treino as T  # noqa: E402

total = falhas = 0


def checar(c, d, det=""):
    global total, falhas
    total += 1
    if c:
        print("ok   " + d)
    else:
        falhas += 1
        print("FALHA " + d + ("\n      " + str(det)[:600] if det else ""))


def sha(s):
    return hashlib.sha256(s.encode()).hexdigest()


def trajetoria(id_, repo="r1", concluido=True, val_seq=5, patch_seq=3, resultado_t="aplicada: aprovada e gravada", pedido=None,
               licenca="proprio", proven="código do autor", verificacao=None, resposta='{"edicoes":[{"acao":"editar"}]}', prompt="corrija soma em soma.c"):
    arts = {sha(prompt): prompt, sha(resposta): resposta, sha("recusada"): "recusada"}
    return {
        "versao": "lia-trajetoria/1", "id": id_, "licenca": licenca, "proveniencia": proven, "repositorio": repo,
        "pedido": pedido or ("corrija a função soma em soma.c do projeto " + id_),
        "resultado": {"concluido": concluido, "causa": "concluido" if concluido else "validacao", "editou": True,
                      "validou_depois_da_ultima_edicao": concluido, "validacao_passou": concluido, "gravacoes_aprovadas": 1},
        "arvore": {"versao": "lia-arvore-tarefas/1", "id": id_, "nos": [
            {"id": "raiz", "pai": None, "evidencias": [{"tipo": "validacao", "seq": val_seq, "passou": concluido}], "tentativas": []},
            {"id": "proposta-1", "pai": "raiz", "evidencias": [{"tipo": "patch", "seq": patch_seq, "passou": True}],
             "tentativas": [{"id": "proposta-1.t1", "prompt": sha(prompt), "artefato": sha("recusada"), "resultado": "recusada pelo verificador: falta \"conteudo\""},
                            {"id": "proposta-1.t2", "prompt": sha(prompt), "artefato": sha(resposta), "resultado": resultado_t}]}]},
        "artefatos": arts,
        **({"verificacao": verificacao} if verificacao is not None else {}),
    }


def main():
    base = tempfile.mkdtemp(prefix="lia-treino-")
    try:
        ok = trajetoria("a1")
        checar(T.validar(ok) == [], "trajetória completa é aceita", T.validar(ok))
        for campo, val, esperado in (("licenca", None, "licença"), ("licenca", "GPL-desconhecida", "licença"), ("proveniencia", "", "proveniência"),
                                     ("versao", "lia-trajetoria/0", "versão")):
            t = copy.deepcopy(ok)
            t[campo] = val
            checar(any(esperado in m for m in T.validar(t)), "recusa: %s=%r" % (campo, val), T.validar(t))
        t = copy.deepcopy(ok)
        del t["artefatos"][sha("corrija soma em soma.c")]
        checar(any("artefato ausente" in m for m in T.validar(t)), "recusa: tentativa referencia artefato que não veio")

        ex = T.rotular(ok)
        rot = {e["tentativa"]: e["rotulo"] for e in ex}
        checar(rot == {"proposta-1.t1": "negativo", "proposta-1.t2": "formato"}, "rótulos: recusada é negativo; aplicada+validada só pela suíte visível é 'formato'", rot)
        # --- auditoria 2026-11-25: "concluído" + suíte visível NÃO confirma o requisito
        INDEP = {"tipo": "independente", "oraculo_sha256": "e" * 64, "referencia": "avaliacao-x/ho-1", "passou": True}
        HUM = {"tipo": "revisao_humana", "revisor": "autor do código", "data": "2026-11-25", "aprovado": True}
        rv = {e["tentativa"]: (e["rotulo"], e["porque"]) for e in T.rotular(ok)}
        checar(rv["proposta-1.t2"][0] == "formato" and "requisito não confirmado" in rv["proposta-1.t2"][1],
               "sem verificação independente: aplicada+suíte visível vira 'formato' (não 'positivo')", rv)
        checar({e["tentativa"]: e["rotulo"] for e in T.rotular(trajetoria("v1", verificacao=INDEP))}["proposta-1.t2"] == "positivo",
               "verificação independente aprovada: positivo")
        checar({e["tentativa"]: e["rotulo"] for e in T.rotular(trajetoria("v2", verificacao=HUM))}["proposta-1.t2"] == "positivo",
               "revisão humana aprovada: positivo")
        falso = T.rotular(trajetoria("v3", verificacao=dict(INDEP, passou=False)))
        checar({e["tentativa"]: e["rotulo"] for e in falso}["proposta-1.t2"] == "negativo" and any("falso" in e["porque"] for e in falso),
               "falso 'concluído' (oráculo independente reprovou): negativo, nunca positivo", falso)
        for nome, v in (("tipo inventado", {"tipo": "confia-em-mim"}), ("independente sem hash do oráculo", {"tipo": "independente", "passou": True}),
                        ("independente sem 'passou'", {"tipo": "independente", "oraculo_sha256": "e" * 64, "referencia": "x"}),
                        ("revisão sem data", {"tipo": "revisao_humana", "revisor": "x", "aprovado": True})):
            checar(any("verificação" in m for m in T.validar(trajetoria("v4", verificacao=v))), "atestado inválido recusado: " + nome)
        antes = trajetoria("a2", val_seq=2, patch_seq=3)   # validação ANTES do patch
        checar(all(e["rotulo"] == "negativo" for e in T.rotular(antes)), "validação anterior ao último patch: nenhum positivo")
        nc = trajetoria("a3", concluido=False)
        checar(all(e["rotulo"] == "negativo" for e in T.rotular(nc)), "tarefa não concluída: proposta aplicada não vira positivo")
        trunc = trajetoria("a4", resultado_t="cortada no limite de tokens (truncamento)")
        checar(all(e["rotulo"] == "negativo" for e in T.rotular(trunc)), "resposta cortada nunca é positivo")

        txt, n = T.anonimizar("senha = 'hunter2abc'\nemail ana@escola.com.br tel (21) 99999-8888 cpf 123.456.789-09\n"
                              "AKIAABCDEFGHIJKLMNOP /home/fulano/projeto/x.py ghp_" + "a" * 36)
        checar(n >= 7 and "hunter2abc" not in txt and "ana@escola" not in txt and "99999-8888" not in txt and "123.456.789-09" not in txt and
               "AKIA" not in txt and "fulano" not in txt and "ghp_" not in txt, "anonimização: senha, e-mail, telefone, CPF, chaves e home", txt)

        cong = os.path.join(base, "congelado.json")
        json.dump({"tarefas": [{"id": "x", "prompt": "Corrija _iter_log em core.py para preservar o campo detail inteiro mesmo quando ele contém |"}]}, open(cong, "w"))
        c = T.carregar_congelados([cong])
        checar(T.vaza_congelado("corrija _iter_log em core.py para preservar o campo detail inteiro mesmo quando ele contem |", c) and
               T.vaza_congelado("Corrija _iter_log em core.py para preservar o campo detail inteiro quando contém |", c) and
               not T.vaza_congelado("crie um parser de CSV em lib/csv.ts", c), "vazamento do congelado: igual ou parecido é detectado; diferente não")

        ent = os.path.join(base, "ent")
        os.makedirs(ent)
        trajs = [trajetoria("t%d" % i, repo="repo%d" % (i % 5), verificacao=INDEP) for i in range(20)]
        trajs.append(trajetoria("sem-lic", licenca=None))
        trajs.append(trajetoria("vaza", pedido="Corrija _iter_log em core.py para preservar o campo detail inteiro mesmo quando ele contém |"))
        for t in trajs:
            json.dump(t, open(os.path.join(ent, t["id"] + ".json"), "w"))
        open(os.path.join(ent, "quebrado.json"), "w").write("{")
        arqs = [os.path.join(ent, f) for f in os.listdir(ent)]
        s1, s2 = os.path.join(base, "s1"), os.path.join(base, "s2")
        m1 = T.construir(arqs, s1, "Qwen2.5-Coder-1.5B-Instruct", "qwen2.5", "chatml", 7, {"lr": "2e-4", "r": "16"}, [cong])
        m2 = T.construir(list(reversed(arqs)), s2, "Qwen2.5-Coder-1.5B-Instruct", "qwen2.5", "chatml", 7, {"r": "16", "lr": "2e-4"}, [cong])
        checar(m1["conjunto_sha256"] == m2["conjunto_sha256"] and open(os.path.join(s1, "manifesto.json")).read() == open(os.path.join(s2, "manifesto.json")).read(),
               "construção reprodutível: mesma entrada (em outra ordem) -> mesmos hashes e manifesto")
        rej = [json.loads(l) for l in open(os.path.join(s1, "rejeitadas.jsonl"))]
        mot = {r.get("id") or r["arquivo"]: " ".join(r["motivos"]) for r in rej}
        checar("licença" in mot.get("sem-lic", "") and "congelado" in mot.get("vaza", "") and "JSON inválido" in mot.get("quebrado.json", ""),
               "rejeitadas: sem licença, vazamento do congelado e JSON quebrado, com motivo", mot)
        part = {}
        for nome in ("treino", "dev", "teste"):
            for l in open(os.path.join(s1, nome + ".jsonl")):
                r = json.loads(l)
                part.setdefault(r["meta"]["repositorio"], set()).add(nome)
        checar(all(len(v) == 1 for v in part.values()), "partição por repositório: nenhum repositório em duas partições", part)
        pos = sum(1 for n in ("treino", "dev", "teste") for l in open(os.path.join(s1, n + ".jsonl")) if json.loads(l)["meta"]["rotulo"] == "positivo")
        tot = sum(1 for n in ("treino", "dev", "teste") for _ in open(os.path.join(s1, n + ".jsonl")))
        neg = [json.loads(l) for l in open(os.path.join(s1, "negativos.jsonl"))]
        checar(pos == tot == 20 and len(neg) == 20 and all(n["meta"]["rotulo"] == "negativo" for n in neg),
               "treino/dev/teste só com positivos; recusadas vão para negativos.jsonl", (pos, tot, len(neg)))
        checar(m1["base"] == "Qwen2.5-Coder-1.5B-Instruct" and m1["template"] == "chatml" and m1["semente"] == 7 and m1["licencas"] == {"proprio": 20}
               and m1["hiperparametros"] == {"lr": "2e-4", "r": "16"} and len(m1["conjunto_sha256"]) == 64,
               "manifesto: base, tokenizer, template, semente, hiperparâmetros, licenças e hash do conjunto", m1)
        # --- manifesto honesto sobre generalização e aptidão
        checar("particoes" in m1 and "avisos" in m1 and m1.get("apto_para_adaptador") is False and m1.get("avaliacao_de_generalizacao") == "nao_demonstravel",
               "manifesto com 20 positivos de 5 repositórios: avisos, partições contadas, NÃO apto e generalização não demonstrável", m1.get("avisos"))
        checar(any("vazia" in x or "sem " in x for x in m1["avisos"]) and sum(p["positivos"] for p in m1["particoes"].values()) == 20,
               "avisos citam partição vazia/sem diversidade; contagens por partição somam os positivos", m1["particoes"])
        so_vis = [trajetoria("sv%d" % i, repo="r%d" % i) for i in range(6)]
        d_sv = os.path.join(base, "sv"); os.makedirs(d_sv)
        for t in so_vis:
            json.dump(t, open(os.path.join(d_sv, t["id"] + ".json"), "w"))
        msv = T.construir([os.path.join(d_sv, f) for f in os.listdir(d_sv)], os.path.join(base, "sv-out"), "b", "t", "chatml", 1, {}, [])
        checar(msv["arquivos"]["treino.jsonl"]["registros"] == 0 and msv["arquivos"]["dev.jsonl"]["registros"] == 0 and
               msv["arquivos"]["teste.jsonl"]["registros"] == 0 and msv["arquivos"]["formato.jsonl"]["registros"] == 6 and msv["apto_para_adaptador"] is False,
               "trajetórias só com validação visível: zero positivos em treino/dev/teste; vão para formato.jsonl; não apto", msv["arquivos"])
        # grande e diverso: com muitos repositórios o manifesto pode declarar a aptidão (limiares fixados no código)
        grande = os.path.join(base, "grande"); os.makedirs(grande)
        for i in range(2 * T.MIN_POSITIVOS_TREINO + 100):
            json.dump(trajetoria("g%d" % i, repo="rg%d" % (i % 100), verificacao=INDEP, pedido="tarefa distinta número %d sobre o módulo m%d" % (i, i)),
                      open(os.path.join(grande, "g%d.json" % i), "w"))
        mg = T.construir([os.path.join(grande, f) for f in os.listdir(grande)], os.path.join(base, "grande-out"), "b", "t", "chatml", 3, {}, [])
        checar(mg["apto_para_adaptador"] is True and mg["avaliacao_de_generalizacao"] == "possivel" and not mg["avisos"],
               "conjunto grande e diverso (partições não vazias): sem avisos e apto", (mg["avisos"], mg["particoes"]))
        # --- congelados: por repositório e por identificadores, não só por texto
        cong2 = os.path.join(base, "cong2.json")
        json.dump({"tarefas": [{"id": "x", "projeto": "/home/zad/sf", "subdir": "",
                                "prompt": "Em core.py, faça normalise_phone descartar o ramal antes de normalizar: tudo a partir da palavra ramal é ignorado."}]}, open(cong2, "w"))
        c2 = T.carregar_congelados([cong2])
        parafrase = "Preciso que normalise_phone, lá no core.py, ignore extensões telefônicas (ramal) e siga como antes"
        checar(not T.vaza_congelado("faça um parser de CSV em lib/csv.ts", c2) and T.vaza_congelado(parafrase, c2),
               "paráfrase com os mesmos identificadores (normalise_phone, core.py) é detectada, mesmo com Jaccard baixo", T.vaza_congelado(parafrase, c2))
        repo_sf = hashlib.sha256(b"/home/zad/sf").hexdigest()
        tr_repo = trajetoria("rp", repo=repo_sf, verificacao=INDEP, pedido="tarefa completamente diferente de tudo")
        checar(any("congelado" in m for m in T.validar(tr_repo, c2)), "trajetória de repositório que tem tarefa congelada é recusada", T.validar(tr_repo, c2))
        checar(T.limites_do_detector() and "paráfrase" in " ".join(T.limites_do_detector()), "limites do detector de similaridade documentados no código")

        # comando atestar: grava numa cópia, valida o atestado, nunca sobrescreve a original
        orig = os.path.join(base, "orig.json"); copia = os.path.join(base, "atestada.json")
        json.dump(trajetoria("at1"), open(orig, "w"))
        antes_b = open(orig, "rb").read()
        checar(T.main(["atestar", orig, "--saida", orig, "--tipo", "revisao_humana", "--revisor", "autor", "--data", "2026-11-25", "--aprovado", "sim"]) == 2
               and open(orig, "rb").read() == antes_b, "atestar: recusa sobrescrever a trajetória original")
        checar(T.main(["atestar", orig, "--saida", copia, "--tipo", "independente", "--passou", "sim"]) == 2 and not os.path.exists(copia),
               "atestar: atestado independente sem hash do oráculo/referência é recusado e nada é gravado")
        checar(T.main(["atestar", orig, "--saida", copia, "--tipo", "independente", "--oraculo-sha256", "a" * 64, "--referencia", "aval/ho-1", "--passou", "sim"]) == 0
               and T.validar(json.load(open(copia))) == [] and open(orig, "rb").read() == antes_b
               and {e["tentativa"]: e["rotulo"] for e in T.rotular(json.load(open(copia)))}["proposta-1.t2"] == "positivo",
               "atestar: grava a cópia atestada (vira positivo) e preserva a original")

        mm = {"formato": "lia-modelo/1", "id": "lia-coding", "versao": "0", "tokenizer": "llama3", "template": "llama3",
              "formato_inferencia": "gguf q4_k_m", "backend_testado": "ollama", "data": "2026-11-24", "estado": "padrao-vigente",
              "base": {"nome": "llama3.2:3b", "licenca": "Llama 3.2 Community License", "sha256": "a" * 64}, "adaptador": None,
              "memoria_minima_medida_mb": 2900, "metricas": {"conjunto_sha256": "b" * 64}, "rollback": {"procedimento": "voltar LIA_OLLAMA_MODELO"}}
        checar(T.validar_manifesto_modelo(mm) == [], "manifesto de modelo completo é aceito", T.validar_manifesto_modelo(mm))
        m2 = dict(mm, adaptador={"sha256": "c" * 64, "formato": "gguf-lora"})
        checar(any("treino_manifesto" in x for x in T.validar_manifesto_modelo(m2)), "adaptador sem manifesto de treino é recusado")
        m3 = dict(mm, base={"nome": "x"})
        checar(any("base precisa" in x for x in T.validar_manifesto_modelo(m3)), "manifesto sem hash/licença da base é recusado")
        m4 = dict(mm, estado="oficial")
        checar(any("promovido_por" in x for x in T.validar_manifesto_modelo(m4)), "'oficial' sem ADR de promoção é recusado")
        m5 = dict(mm, memoria_minima_medida_mb=None)
        checar(any("memoria" in x for x in T.validar_manifesto_modelo(m5)), "memória não medida é recusada")
        amb = T.verificar_ambiente()
        checar(isinstance(amb.get("suficiente"), bool) and ("falta" in amb), "verificar-ambiente responde (suficiente/falta)", amb)
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print("\n%d testes, %d falhas" % (total, falhas))
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
