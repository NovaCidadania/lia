#!/usr/bin/env python3
"""Pipeline de dados de treino da Lia (trajetórias verificadas -> conjunto
versionado com manifesto). Python puro, sem dependências.

  lia_treino.py validar   ARQ.json...
  lia_treino.py construir --entrada DIR --saida DIR --base NOME --tokenizer NOME
                          --template chatml --semente N [--congelados ARQ.json ...]
                          [--hiper chave=valor ...]
  lia_treino.py verificar-ambiente [--base-params 1.5e9]

Regras (testadas em testes/treino/teste_pipeline.py):
- trajetória só entra com versão conhecida, licença aceita e proveniência;
- POSITIVO só de proposta aplicada (diff aprovado pela pessoa) numa tarefa
  concluída, com a validação passando DEPOIS do último patch (pela árvore);
- proposta recusada/cortada/rejeitada vira NEGATIVO (reparo), nunca positivo;
- segredos, e-mails, telefones, CPFs e caminhos de home são anonimizados;
- partição treino/dev/teste por REPOSITÓRIO (sem vazamento entre variantes);
- enunciados dos conjuntos congelados nunca entram (igual ou muito parecido);
- manifesto com base, tokenizer, template, hash do conjunto, licenças,
  proveniência, semente e hiperparâmetros: a mesma entrada gera os mesmos bytes.
"""
import argparse, hashlib, json, os, re, sys, unicodedata

VERSAO_TRAJETORIA = "lia-trajetoria/1"
VERSAO_ARVORE = "lia-arvore-tarefas/1"
VERSAO_PIPELINE = "lia-treino/1"
# código do próprio usuário ("proprio") ou licenças que permitem uso para treino
LICENCAS_ACEITAS = {"proprio", "MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "ISC", "MPL-2.0", "Unlicense", "CC0-1.0", "0BSD"}


# ------------------------------------------------------------------ validação
def validar(t):
    """lista de motivos de recusa (vazia = trajetória aceitável)"""
    e = []
    if not isinstance(t, dict):
        return ["não é um objeto"]
    if t.get("versao") != VERSAO_TRAJETORIA:
        e.append("versão desconhecida: %r" % t.get("versao"))
    for k in ("id", "pedido", "repositorio"):
        if not isinstance(t.get(k), str) or not t.get(k):
            e.append("campo obrigatório ausente: %s" % k)
    if t.get("licenca") not in LICENCAS_ACEITAS:
        e.append("licença ausente ou não aceita para treino: %r" % t.get("licenca"))
    if not isinstance(t.get("proveniencia"), str) or not t.get("proveniencia").strip():
        e.append("proveniência ausente")
    r = t.get("resultado")
    if not isinstance(r, dict) or not isinstance(r.get("concluido"), bool):
        e.append("resultado ausente ou sem 'concluido'")
    a = t.get("arvore")
    if not isinstance(a, dict) or a.get("versao") != VERSAO_ARVORE or not isinstance(a.get("nos"), list):
        e.append("árvore ausente ou de versão desconhecida")
    arts = t.get("artefatos")
    if not isinstance(arts, dict):
        e.append("artefatos ausentes")
        arts = {}
    for n in (a or {}).get("nos", []) if isinstance(a, dict) else []:
        for x in n.get("tentativas", []):
            for ref in ("artefato", "prompt"):
                if x.get(ref) and x[ref] not in arts:
                    e.append("tentativa %s referencia artefato ausente (%s)" % (x.get("id"), ref))
    return e


def _nos(t):
    return {n["id"]: n for n in t["arvore"]["nos"]}


def validacao_depois_do_ultimo_patch(t):
    """a última validação da raiz passou e é posterior a TODO patch da árvore"""
    nos = _nos(t)
    patches = [ev["seq"] for n in nos.values() for ev in n.get("evidencias", []) if ev.get("tipo") == "patch"]
    vals = [ev for ev in nos.get("raiz", {}).get("evidencias", []) if ev.get("tipo") == "validacao"]
    if not patches or not vals:
        return False
    ultima = max(vals, key=lambda v: v["seq"])
    return bool(ultima.get("passou")) and ultima["seq"] > max(patches)


# ------------------------------------------------------------------ rótulos
def rotular(t):
    """exemplos (prompt, resposta) com rótulo 'positivo' ou 'negativo' e o porquê"""
    r = t["resultado"]
    tarefa_ok = (r.get("concluido") is True and r.get("validou_depois_da_ultima_edicao") is True and r.get("validacao_passou") is True
                 and (r.get("gravacoes_aprovadas") or 0) > 0 and validacao_depois_do_ultimo_patch(t))
    out = []
    for n in t["arvore"]["nos"]:
        for x in n.get("tentativas", []):
            if not x.get("prompt") or not x.get("artefato"):
                continue  # receita determinística (sem prompt): não é exemplo de modelo
            res = x.get("resultado") or ""
            if res.startswith("aplicada") and tarefa_ok:
                rot, porque = "positivo", "aplicada (diff aprovado) e a validação passou depois do último patch"
            elif res.startswith("aplicada"):
                rot, porque = "negativo", "aplicada, mas a tarefa não foi concluída com validação depois do último patch"
            else:
                rot, porque = "negativo", res or "sem resultado"
            out.append({"tentativa": x["id"], "rotulo": rot, "porque": porque,
                        "prompt": t["artefatos"][x["prompt"]], "resposta": t["artefatos"][x["artefato"]]})
    return out


# ------------------------------------------------------------------ anonimização
_SUBST = [
    (re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?-----END [A-Z ]*PRIVATE KEY-----", re.S), "«chave privada omitida»"),
    (re.compile(r"\bAKIA[0-9A-Z]{16}\b"), "«omitido»"),
    (re.compile(r"\bgh[pousr]_[A-Za-z0-9]{30,}\b"), "«omitido»"),
    (re.compile(r"\bsk-[A-Za-z0-9_-]{20,}\b"), "«omitido»"),
    (re.compile(r"(?i)\b(password|passwd|senha|secret|token|api[_-]?key)(\s*[:=]\s*)(['\"]?)[^\s'\"]{4,}\3"), r"\1\2\3«omitido»\3"),
    (re.compile(r"[\w.+-]+@[\w-]+(\.[\w-]+)+"), "<email>"),
    (re.compile(r"\b\d{3}\.\d{3}\.\d{3}-\d{2}\b"), "<cpf>"),
    (re.compile(r"(?<!\d)(\+?55\s?)?\(?\d{2}\)?[\s-]?9?\d{4}[\s-]?\d{4}(?!\d)"), "<telefone>"),
    (re.compile(r"/(home|Users)/[^/\s'\"]+"), r"/\1/<usuario>"),
]


def anonimizar(texto):
    n = 0
    for rx, sub in _SUBST:
        texto, k = rx.subn(sub, texto)
        n += k
    return texto, n


# ------------------------------------------------------------------ vazamento
def _norm(s):
    s = unicodedata.normalize("NFKD", s or "").encode("ascii", "ignore").decode().lower()
    return " ".join(re.findall(r"[a-z0-9_]+", s))


def _jaccard(a, b):
    A, B = set(a.split()), set(b.split())
    return len(A & B) / max(1, len(A | B))


def carregar_congelados(caminhos):
    p = []
    for c in caminhos or []:
        for t in json.load(open(c)).get("tarefas", []):
            p.append(_norm(t["prompt"]))
    return p


def vaza_congelado(pedido, congelados, limiar=0.6):
    n = _norm(pedido)
    return any(n == c or _jaccard(n, c) >= limiar for c in congelados)


def particao(repositorio, semente):
    h = int(hashlib.sha256(("%s|%s" % (semente, repositorio)).encode()).hexdigest(), 16) % 10
    return "teste" if h == 0 else "dev" if h == 1 else "treino"


# ------------------------------------------------------------------ construção
def _sha(b):
    return hashlib.sha256(b).hexdigest()


def construir(arquivos, saida, base, tokenizer, template, semente, hiper, congelados_caminhos):
    congelados = carregar_congelados(congelados_caminhos)
    os.makedirs(saida, exist_ok=True)
    conj = {"treino": [], "dev": [], "teste": []}
    negativos, rejeitadas = [], []
    licencas, proven = {}, set()
    anonimizados = 0
    for arq in sorted(arquivos):
        try:
            t = json.load(open(arq))
        except Exception as ex:  # noqa: BLE001
            rejeitadas.append({"arquivo": os.path.basename(arq), "motivos": ["JSON inválido: %s" % ex]})
            continue
        mot = validar(t)
        if not mot and vaza_congelado(t["pedido"], congelados):
            mot = ["enunciado igual ou muito parecido com um do conjunto congelado de avaliação"]
        if mot:
            rejeitadas.append({"id": t.get("id") if isinstance(t, dict) else None, "arquivo": os.path.basename(arq), "motivos": mot})
            continue
        licencas[t["licenca"]] = licencas.get(t["licenca"], 0) + 1
        proven.add(t["proveniencia"])
        parte = particao(t["repositorio"], semente)
        for ex in rotular(t):
            p, k1 = anonimizar(ex["prompt"])
            r, k2 = anonimizar(ex["resposta"])
            anonimizados += k1 + k2
            reg = {"messages": [{"role": "user", "content": p}, {"role": "assistant", "content": r}],
                   "meta": {"trajetoria": t["id"], "tentativa": ex["tentativa"], "rotulo": ex["rotulo"], "porque": ex["porque"],
                            "repositorio": t["repositorio"], "licenca": t["licenca"], "particao": parte}}
            (conj[parte] if ex["rotulo"] == "positivo" else negativos).append(reg)
    arquivos_saida = {}
    for nome, regs in (("treino.jsonl", conj["treino"]), ("dev.jsonl", conj["dev"]), ("teste.jsonl", conj["teste"]),
                       ("negativos.jsonl", negativos), ("rejeitadas.jsonl", rejeitadas)):
        regs = sorted(regs, key=lambda x: json.dumps(x, sort_keys=True, ensure_ascii=False))
        dados = "".join(json.dumps(x, sort_keys=True, ensure_ascii=False) + "\n" for x in regs).encode()
        open(os.path.join(saida, nome), "wb").write(dados)
        arquivos_saida[nome] = {"sha256": _sha(dados), "registros": len(regs)}
    conjunto_sha = _sha("".join(sorted(v["sha256"] for k, v in arquivos_saida.items() if k != "rejeitadas.jsonl")).encode())
    manifesto = {
        "formato": VERSAO_PIPELINE, "base": base, "tokenizer": tokenizer, "template": template, "semente": semente,
        "hiperparametros": dict(sorted(hiper.items())), "conjunto_sha256": conjunto_sha, "arquivos": arquivos_saida,
        "licencas": dict(sorted(licencas.items())), "proveniencias": sorted(proven), "substituicoes_anonimizacao": anonimizados,
        "congelados_excluidos": [os.path.basename(c) for c in (congelados_caminhos or [])],
        "particao": "por repositório: sha256(semente|repositorio) % 10 -> 0 teste, 1 dev, resto treino",
        "observacao": "positivos só de propostas aplicadas em tarefas concluídas com validação depois do último patch; "
                      "negativos (recusadas/cortadas/não concluídas) nunca entram em treino.jsonl",
    }
    open(os.path.join(saida, "manifesto.json"), "w").write(json.dumps(manifesto, sort_keys=True, ensure_ascii=False, indent=1) + "\n")
    return manifesto


# ------------------------------------------------------------------ modelo oficial
VERSAO_MODELO = "lia-modelo/1"


def validar_manifesto_modelo(m):
    """manifesto do modelo oficial (docs/modelos/*.json): motivos de recusa"""
    e = []
    if not isinstance(m, dict) or m.get("formato") != VERSAO_MODELO:
        return ["formato desconhecido (esperado %s)" % VERSAO_MODELO]
    for k in ("id", "versao", "tokenizer", "template", "formato_inferencia", "backend_testado", "data", "estado"):
        if not isinstance(m.get(k), str) or not m[k].strip():
            e.append("campo obrigatório ausente: %s" % k)
    b = m.get("base")
    if not isinstance(b, dict) or not all(isinstance(b.get(k), str) and b[k] for k in ("nome", "licenca", "sha256")):
        e.append("base precisa de nome, licenca e sha256 (o adaptador nunca substitui a base)")
    ad = m.get("adaptador")
    if ad is not None and (not isinstance(ad, dict) or not all(isinstance(ad.get(k), str) and ad[k] for k in ("sha256", "formato", "treino_manifesto_sha256"))):
        e.append("adaptador precisa de sha256, formato e treino_manifesto_sha256 (ou null)")
    if not isinstance(m.get("memoria_minima_medida_mb"), int) or m["memoria_minima_medida_mb"] <= 0:
        e.append("memoria_minima_medida_mb precisa ser medida (inteiro > 0)")
    met = m.get("metricas")
    if not isinstance(met, dict) or not isinstance(met.get("conjunto_sha256"), str):
        e.append("metricas precisam citar o conjunto de avaliação (conjunto_sha256)")
    rb = m.get("rollback")
    if not isinstance(rb, dict) or not rb.get("procedimento"):
        e.append("rollback.procedimento obrigatório")
    if m.get("estado") == "oficial" and not (isinstance(met, dict) and met.get("promovido_por")):
        e.append("estado 'oficial' exige metricas.promovido_por (ADR da promoção)")
    return e


# ------------------------------------------------------------------ ambiente
def verificar_ambiente(base_params=1.5e9):
    """o que o piloto LoRA/QLoRA precisaria e o que existe nesta máquina"""
    rel = {"python": sys.version.split()[0]}
    try:
        mem = {l.split(":")[0]: int(l.split()[1]) for l in open("/proc/meminfo") if l.split(":")[0] in ("MemTotal", "MemAvailable")}
        rel["ram_total_gb"] = round(mem["MemTotal"] / 1048576, 1)
        rel["ram_livre_gb"] = round(mem["MemAvailable"] / 1048576, 1)
    except Exception:  # noqa: BLE001
        rel["ram_total_gb"] = None
    import importlib.util
    rel["bibliotecas"] = {m: importlib.util.find_spec(m) is not None for m in ("torch", "transformers", "peft", "trl", "datasets", "bitsandbytes")}
    import shutil, subprocess
    gpu = None
    if shutil.which("nvidia-smi"):
        r = subprocess.run(["nvidia-smi", "--query-gpu=name,memory.total", "--format=csv,noheader"], capture_output=True, text=True)
        gpu = r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None
    rel["gpu_cuda"] = gpu
    falta = []
    if not gpu:
        falta.append("nenhuma GPU CUDA utilizável (QLoRA de %.1fB precisa de GPU com >= 8-12 GB)" % (base_params / 1e9))
    if not all(rel["bibliotecas"][m] for m in ("torch", "transformers", "peft")):
        falta.append("torch/transformers/peft não instalados (não instalo nada sem autorização)")
    if (rel.get("ram_total_gb") or 0) < 16 and not gpu:
        falta.append("RAM total < 16 GB para treino em CPU, mesmo de modelo pequeno")
    rel["suficiente"] = not falta
    rel["falta"] = falta
    return rel


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("validar"); v.add_argument("arquivos", nargs="+")
    c = sub.add_parser("construir")
    c.add_argument("--entrada", required=True); c.add_argument("--saida", required=True)
    c.add_argument("--base", required=True); c.add_argument("--tokenizer", required=True); c.add_argument("--template", required=True)
    c.add_argument("--semente", type=int, required=True); c.add_argument("--congelados", nargs="*", default=[])
    c.add_argument("--hiper", nargs="*", default=[])
    e = sub.add_parser("verificar-ambiente"); e.add_argument("--base-params", type=float, default=1.5e9)
    vm = sub.add_parser("validar-modelo"); vm.add_argument("arquivos", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "validar":
        ruim = 0
        for f in a.arquivos:
            m = validar(json.load(open(f)))
            print("%s: %s" % (f, "ok" if not m else "; ".join(m)))
            ruim += bool(m)
        return 1 if ruim else 0
    if a.cmd == "validar-modelo":
        ruim = 0
        for f in a.arquivos:
            m = validar_manifesto_modelo(json.load(open(f)))
            print("%s: %s" % (f, "ok" if not m else "; ".join(m)))
            ruim += bool(m)
        return 1 if ruim else 0
    if a.cmd == "construir":
        arqs = [os.path.join(a.entrada, f) for f in os.listdir(a.entrada) if f.endswith(".json")]
        hiper = dict(x.split("=", 1) for x in a.hiper)
        m = construir(arqs, a.saida, a.base, a.tokenizer, a.template, a.semente, hiper, a.congelados)
        print(json.dumps({k: m[k] for k in ("conjunto_sha256", "arquivos")}, ensure_ascii=False, indent=1))
        return 0
    rel = verificar_ambiente(a.base_params)
    print(json.dumps(rel, ensure_ascii=False, indent=1))
    return 0 if rel["suficiente"] else 3


if __name__ == "__main__":
    sys.exit(main())
