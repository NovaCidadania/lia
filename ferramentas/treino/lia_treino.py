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
- POSITIVO (requisito resolvido) exige, além da proposta aplicada numa tarefa
  concluída com validação depois do último patch, uma VERIFICAÇÃO que não é
  a suíte visível: "independente" (oráculo fora do agente) ou "revisao_humana".
  Sem esse atestado a proposta vira "formato" (serve só para aprender o
  formato/reparo; o requisito não foi confirmado) e vai para formato.jsonl;
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
TIPOS_VERIFICACAO = ("validacao_visivel", "revisao_humana", "independente")
# limiares fixados no código (não ajustáveis por quem treina): abaixo deles o manifesto não declara aptidão
MIN_POSITIVOS_TREINO = 200      # positivos CONFIRMADOS na partição de treino
MIN_REPOS_TREINO = 5            # repositórios distintos no treino
LICENCAS_ACEITAS = {"proprio", "MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "ISC", "MPL-2.0", "Unlicense", "CC0-1.0", "0BSD"}


# ------------------------------------------------------------------ validação
def validar(t, congelados=None):
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
    e.extend(validar_verificacao(t.get("verificacao")))
    if congelados is not None and isinstance(t.get("repositorio"), str) and t["repositorio"] in congelados.repos:
        e.append("repositório de uma tarefa do conjunto congelado de avaliação: nunca entra em treino")
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


def validar_verificacao(v):
    """atestado de como o requisito foi confirmado. Ausente = só a validação visível (o único que o runtime atesta)."""
    if v is None:
        return []
    if not isinstance(v, dict) or v.get("tipo") not in TIPOS_VERIFICACAO:
        return ["verificação: tipo desconhecido (esperado um de %s)" % ", ".join(TIPOS_VERIFICACAO)]
    e = []
    if v["tipo"] == "independente":
        if not (isinstance(v.get("oraculo_sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", v["oraculo_sha256"])):
            e.append("verificação independente sem oraculo_sha256 (hash do oráculo executado fora do agente)")
        if not (isinstance(v.get("referencia"), str) and v["referencia"].strip()):
            e.append("verificação independente sem referencia (execução/avaliação que a produziu)")
        if not isinstance(v.get("passou"), bool):
            e.append("verificação independente sem 'passou' (booleano)")
    elif v["tipo"] == "revisao_humana":
        if not (isinstance(v.get("revisor"), str) and v["revisor"].strip()):
            e.append("revisão humana sem revisor (papel, não nome)")
        if not (isinstance(v.get("data"), str) and re.fullmatch(r"\d{4}-\d{2}-\d{2}", v["data"])):
            e.append("revisão humana sem data AAAA-MM-DD")
        if not isinstance(v.get("aprovado"), bool):
            e.append("revisão humana sem 'aprovado' (booleano)")
    return ["verificação: " + x for x in e] if e else []


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
def _confirmacao(t):
    """(tipo, resultado) da verificação do REQUISITO: resultado True/False, ou None se só a suíte visível o atesta"""
    v = t.get("verificacao") or {}
    if v.get("tipo") == "independente":
        return "independente", v.get("passou")
    if v.get("tipo") == "revisao_humana":
        return "revisão humana", v.get("aprovado")
    return "validacao_visivel", None


def rotular(t):
    """exemplos (prompt, resposta) rotulados:
    positivo  — aplicada, tarefa concluída E requisito confirmado por oráculo independente/revisão humana;
    formato   — aplicada e concluída só pela validação visível (requisito NÃO confirmado: só formato/reparo);
    negativo  — o resto (recusada, cortada, não concluída, ou reprovada pela verificação do requisito)."""
    r = t["resultado"]
    tarefa_ok = (r.get("concluido") is True and r.get("validou_depois_da_ultima_edicao") is True and r.get("validacao_passou") is True
                 and (r.get("gravacoes_aprovadas") or 0) > 0 and validacao_depois_do_ultimo_patch(t))
    como, confirmado = _confirmacao(t)
    out = []
    for n in t["arvore"]["nos"]:
        for x in n.get("tentativas", []):
            if not x.get("prompt") or not x.get("artefato"):
                continue  # receita determinística (sem prompt): não é exemplo de modelo
            res = x.get("resultado") or ""
            if res.startswith("aplicada") and confirmado is False:
                rot, porque = "negativo", "falso 'concluído': aplicada, mas a verificação (%s) reprovou o requisito" % como
            elif res.startswith("aplicada") and tarefa_ok and confirmado is True:
                rot, porque = "positivo", "aplicada, validação depois do último patch e requisito confirmado (%s)" % como
            elif res.startswith("aplicada") and tarefa_ok:
                rot, porque = "formato", "aplicada e a suíte visível passou, mas o requisito não confirmado: serve só para formato/reparo"
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


_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:_[A-Za-z0-9]+|[a-z][A-Z])[A-Za-z0-9_]*|[\w./-]+\.(?:py|ts|tsx|js|jsx|mjs|php|c|h|cs|go|rs|java|rb|sh)\b")


def _identificadores(texto):
    """nomes de código (snake_case, camelCase, arquivos) com >= 6 caracteres, em minúsculas"""
    return {m.group(0).lower() for m in _IDENT.finditer(texto or "") if len(m.group(0)) >= 6}


class Congelados:
    def __init__(self):
        self.prompts, self.idents, self.repos = [], [], set()


def carregar_congelados(caminhos):
    c = Congelados()
    for cam in caminhos or []:
        for t in json.load(open(cam)).get("tarefas", []):
            c.prompts.append(_norm(t["prompt"]))
            c.idents.append(_identificadores(t["prompt"]))
            proj = t.get("projeto")
            if proj:  # o runtime identifica o repositório por sha256 da raiz (com ou sem subdiretório)
                for raiz in {proj, os.path.join(proj, t.get("subdir") or "") if t.get("subdir") else proj}:
                    c.repos.add(hashlib.sha256(raiz.encode()).hexdigest())
    return c


def vaza_congelado(pedido, congelados, limiar=0.6):
    """igual, parecido (Jaccard) ou com >= 2 identificadores de código em comum com um enunciado congelado"""
    n = _norm(pedido)
    ids = _identificadores(pedido)
    return any(n == c or _jaccard(n, c) >= limiar for c in congelados.prompts) or \
        any(len(ids & i) >= 2 for i in congelados.idents)


def limites_do_detector():
    return ["detecta enunciado igual, parecido (Jaccard >= 0.6), >= 2 identificadores de código em comum e o repositório inteiro",
            "NÃO detecta paráfrase semântica sem palavras/identificadores em comum, nem tradução, nem a mesma ideia em outro projeto",
            "só vê o texto do pedido: o código dos artefatos e o conteúdo das verificações ocultas não são comparados",
            "limiares fixados à mão; nunca foram calibrados com exemplos reais"]


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
    negativos, rejeitadas, formato = [], [], []
    repos_aceitos = {"treino": set(), "dev": set(), "teste": set()}
    licencas, proven = {}, set()
    anonimizados = 0
    for arq in sorted(arquivos):
        try:
            t = json.load(open(arq))
        except Exception as ex:  # noqa: BLE001
            rejeitadas.append({"arquivo": os.path.basename(arq), "motivos": ["JSON inválido: %s" % ex]})
            continue
        mot = validar(t, congelados)
        if not mot and vaza_congelado(t["pedido"], congelados):
            mot = ["enunciado igual ou muito parecido com um do conjunto congelado de avaliação"]
        if mot:
            rejeitadas.append({"id": t.get("id") if isinstance(t, dict) else None, "arquivo": os.path.basename(arq), "motivos": mot})
            continue
        licencas[t["licenca"]] = licencas.get(t["licenca"], 0) + 1
        proven.add(t["proveniencia"])
        parte = particao(t["repositorio"], semente)
        repos_aceitos[parte].add(t["repositorio"])
        for ex in rotular(t):
            p, k1 = anonimizar(ex["prompt"])
            r, k2 = anonimizar(ex["resposta"])
            anonimizados += k1 + k2
            reg = {"messages": [{"role": "user", "content": p}, {"role": "assistant", "content": r}],
                   "meta": {"trajetoria": t["id"], "tentativa": ex["tentativa"], "rotulo": ex["rotulo"], "porque": ex["porque"],
                            "repositorio": t["repositorio"], "licenca": t["licenca"], "particao": parte}}
            (conj[parte] if ex["rotulo"] == "positivo" else formato if ex["rotulo"] == "formato" else negativos).append(reg)
    arquivos_saida = {}
    for nome, regs in (("treino.jsonl", conj["treino"]), ("dev.jsonl", conj["dev"]), ("teste.jsonl", conj["teste"]),
                       ("formato.jsonl", formato), ("negativos.jsonl", negativos), ("rejeitadas.jsonl", rejeitadas)):
        regs = sorted(regs, key=lambda x: json.dumps(x, sort_keys=True, ensure_ascii=False))
        dados = "".join(json.dumps(x, sort_keys=True, ensure_ascii=False) + "\n" for x in regs).encode()
        open(os.path.join(saida, nome), "wb").write(dados)
        arquivos_saida[nome] = {"sha256": _sha(dados), "registros": len(regs)}
    particoes = {n: {"repositorios": len(repos_aceitos[n]), "positivos": len(conj[n])} for n in ("treino", "dev", "teste")}
    avisos = []
    for n in ("dev", "teste"):
        if particoes[n]["repositorios"] == 0:
            avisos.append("partição %s vazia: nenhum repositório caiu nela; sem ela não há como avaliar generalização" % n)
        elif particoes[n]["positivos"] == 0:
            avisos.append("partição %s sem positivos confirmados" % n)
    if particoes["treino"]["repositorios"] < MIN_REPOS_TREINO:
        avisos.append("treino com só %d repositório(s) (mínimo %d): sem diversidade" % (particoes["treino"]["repositorios"], MIN_REPOS_TREINO))
    if particoes["treino"]["positivos"] < MIN_POSITIVOS_TREINO:
        avisos.append("treino com só %d positivos confirmados (mínimo %d)" % (particoes["treino"]["positivos"], MIN_POSITIVOS_TREINO))
    conjunto_sha = _sha("".join(sorted(v["sha256"] for k, v in arquivos_saida.items() if k != "rejeitadas.jsonl")).encode())
    manifesto = {
        "formato": VERSAO_PIPELINE, "base": base, "tokenizer": tokenizer, "template": template, "semente": semente,
        "hiperparametros": dict(sorted(hiper.items())), "conjunto_sha256": conjunto_sha, "arquivos": arquivos_saida,
        "licencas": dict(sorted(licencas.items())), "proveniencias": sorted(proven), "substituicoes_anonimizacao": anonimizados,
        "congelados_excluidos": [os.path.basename(c) for c in (congelados_caminhos or [])],
        "limites_do_detector_de_congelados": limites_do_detector(),
        "particoes": particoes, "avisos": avisos,
        "avaliacao_de_generalizacao": "possivel" if not avisos else "nao_demonstravel",
        "apto_para_adaptador": not avisos,
        "limiares": {"min_positivos_treino": MIN_POSITIVOS_TREINO, "min_repos_treino": MIN_REPOS_TREINO},
        "particao": "por repositório: sha256(semente|repositorio) % 10 -> 0 teste, 1 dev, resto treino",
        "observacao": "positivos = proposta aplicada em tarefa concluída com validação depois do último patch E requisito confirmado "
                      "por verificação independente ou revisão humana; só validação visível vai para formato.jsonl (não é prova de que "
                      "o requisito foi resolvido); negativos (recusadas/cortadas/não concluídas/reprovadas) nunca entram em treino.jsonl",
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
    c.add_argument("--exigir-aptidao", action="store_true", help="sai com 4 se o manifesto tiver avisos (não apto para adaptador)")
    at = sub.add_parser("atestar", help="grava o atestado de verificação numa CÓPIA da trajetória")
    at.add_argument("arquivo"); at.add_argument("--saida", required=True)
    at.add_argument("--tipo", required=True, choices=("independente", "revisao_humana"))
    at.add_argument("--oraculo-sha256"); at.add_argument("--referencia"); at.add_argument("--passou", choices=("sim", "nao"))
    at.add_argument("--revisor"); at.add_argument("--data"); at.add_argument("--aprovado", choices=("sim", "nao"))
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
    if a.cmd == "atestar":
        if os.path.abspath(a.arquivo) == os.path.abspath(a.saida):
            print("a saída precisa ser outro arquivo (a trajetória original não é alterada)")
            return 2
        t = json.load(open(a.arquivo))
        if a.tipo == "independente":
            v = {"tipo": "independente", "oraculo_sha256": a.oraculo_sha256, "referencia": a.referencia,
                 "passou": None if a.passou is None else a.passou == "sim"}
        else:
            v = {"tipo": "revisao_humana", "revisor": a.revisor, "data": a.data, "aprovado": None if a.aprovado is None else a.aprovado == "sim"}
        mot = validar_verificacao(v)
        if mot:
            print("; ".join(mot))
            return 2
        t["verificacao"] = v
        open(a.saida, "w").write(json.dumps(t, ensure_ascii=False, indent=1) + "\n")
        print("atestado gravado em %s" % a.saida)
        return 0
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
        print(json.dumps({k: m[k] for k in ("conjunto_sha256", "arquivos", "particoes", "avisos", "apto_para_adaptador")}, ensure_ascii=False, indent=1))
        for av in m["avisos"]:
            print("AVISO: " + av, file=sys.stderr)
        return 4 if (a.exigir_aptidao and not m["apto_para_adaptador"]) else 0
    rel = verificar_ambiente(a.base_params)
    print(json.dumps(rel, ensure_ascii=False, indent=1))
    return 0 if rel["suficiente"] else 3


if __name__ == "__main__":
    sys.exit(main())
