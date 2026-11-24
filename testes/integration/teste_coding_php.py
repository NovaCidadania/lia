#!/usr/bin/env python3
"""Fase 2b: coding em projeto PHP/CodeIgniter-like, Git como pré-condição,
pedidos analíticos e manifests Docker — SEM rede, SEM banco, SEM LLM
(modelo mock roteirizado). Projeto de fixture sob /tmp com repositório Git
próprio. O 'vendor/bin/phpunit' do fixture é um executor mínimo escrito
para este teste: só carrega os arquivos de teste e roda os métodos test*;
não executa comandos. Uso: make teste-coding-php"""
import json, os, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from teste_web_resposta import Conexao, parar  # noqa: E402

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SESSIOND = os.path.join(RAIZ, "build", "lia-sessiond")
total = falhas = 0


def checar(cond, desc, detalhe=""):
    global total, falhas
    total += 1
    if cond:
        print("ok   " + desc, flush=True)
    else:
        falhas += 1
        print("FALHA " + desc + (("\n      " + str(detalhe)[:2000]) if detalhe != "" else ""), flush=True)


MODEL = """<?php
defined('BASEPATH') OR exit('No direct script access allowed');

class M_veiculo extends CI_Model {

    public function getmanutencao($id) {
        $query = "
            select mv.id, mv.descricao
            from manutencao as mv
            where mv.idveiculo = $id
        ";
        return $this->db->query($query)->result();
    }
}
"""

TESTE = """<?php
use PHPUnit\\Framework\\TestCase;

if (!defined('BASEPATH')) define('BASEPATH', __DIR__);
if (!class_exists('CI_Model')) { class CI_Model { public $db; } }
require_once __DIR__ . '/../application/models/M_veiculo.php';

class DbFalso {
    public $sql; public $binds;
    public function query($sql, $binds = array()) { $this->sql = $sql; $this->binds = $binds; return $this; }
    public function result() { return array(); }
}

class M_veiculoTest extends TestCase {
    public function testIdNaoEntraNaSql() {
        $m = new M_veiculo();
        $m->db = new DbFalso();
        $m->getmanutencao('1 OR 1=1');
        $this->assertStringNotContainsString('1 OR 1=1', $m->db->sql);
        $this->assertSame(array('1 OR 1=1'), $m->db->binds);
    }
}
"""

PHPUNIT_MINIMO = r"""<?php
// Executor mínimo de testes para o fixture (não é o PHPUnit): carrega os
// arquivos de teste e roda os métodos test*. Não executa comandos.
namespace PHPUnit\Framework {
    class AssertionFailed extends \Exception {}
    class TestCase {
        public function assertSame($a, $b, $m = '') { if ($a !== $b) throw new AssertionFailed($m ?: 'assertSame falhou: ' . var_export($b, true)); }
        public function assertTrue($c, $m = '') { if ($c !== true) throw new AssertionFailed($m ?: 'assertTrue falhou'); }
        public function assertStringNotContainsString($n, $h, $m = '') { if (strpos((string)$h, $n) !== false) throw new AssertionFailed($m ?: "a SQL contém '$n'"); }
        public function assertStringContainsString($n, $h, $m = '') { if (strpos((string)$h, $n) === false) throw new AssertionFailed($m ?: "não contém '$n'"); }
    }
}
namespace {
    $arqs = array_values(array_filter(array_slice($argv, 1), function ($a) { return $a[0] !== '-'; }));
    if (!$arqs) $arqs = glob(getcwd() . '/tests/*Test.php') ?: array();
    $antes = get_declared_classes();
    foreach ($arqs as $a) require_once $a;
    $falhas = 0; $n = 0;
    foreach (array_diff(get_declared_classes(), $antes) as $c) {
        if (!is_subclass_of($c, 'PHPUnit\Framework\TestCase')) continue;
        foreach (get_class_methods($c) as $m) {
            if (strpos($m, 'test') !== 0) continue;
            $n++;
            try { (new $c)->$m(); echo "ok $c::$m\n"; }
            catch (\Throwable $e) { $falhas++; echo "FALHA $c::$m: " . $e->getMessage() . "\n"; }
        }
    }
    echo "$n teste(s), $falhas falha(s)\n";
    exit(($falhas || !$n) ? 1 : 0);
}
"""

COMPOSE = """services:
  app:
    build: .
    ports:
      - "8080:80"
    depends_on:
      - db
    volumes:
      - ./:/var/www/html
    environment:
      DB_PASSWORD: segredo-do-banco-123
      APP_ENV: desenvolvimento
  db:
    image: postgres:16
    environment:
      POSTGRES_PASSWORD: segredo-do-banco-123
    volumes:
      - dados:/var/lib/postgresql/data
volumes:
  dados:
"""

PLANO = {"criterios": [{"descricao": "os testes passam", "verificacao": "validacao"},
                       {"descricao": "usa placeholder", "verificacao": "contem", "caminho": "application/models/M_veiculo.php", "texto": "mv.idveiculo = ?"}],
         "arquivos_para_ler": [], "validacao": "teste"}
EDICAO = {"edicoes": [
    {"acao": "editar", "caminho": "application/models/M_veiculo.php", "procurar": "where mv.idveiculo = $id", "substituir": "where mv.idveiculo = ?"},
    {"acao": "editar", "caminho": "application/models/M_veiculo.php", "procurar": "$this->db->query($query)", "substituir": "$this->db->query($query, array($id))"},
    {"acao": "criar", "caminho": "tests/M_veiculoTest.php", "conteudo": TESTE}], "explicacao": "bind de parâmetro e teste sem banco"}
PEDIDO = ("Corrija a SQL injection em `M_veiculo::getmanutencao`: substitua a interpolação de `$id` por bind de parâmetro do CodeIgniter, "
          "preserve a consulta e o retorno, e crie um teste PHPUnit isolado sem conexão ao banco que prove que `1 OR 1=1` não entra no SQL.")


def git(proj, *a):
    return subprocess.run(["git", "-C", proj, "-c", "user.name=T", "-c", "user.email=t@t"] + list(a), capture_output=True, text=True).stdout


class Projeto:
    def __init__(self, base, nome, roteiro, corromper_git=False):
        self.dir = os.path.join(base, nome)
        self.proj = os.path.join(self.dir, "proj")
        self.home = os.path.join(self.dir, "home")
        for d in ("application/models", "vendor/bin"):
            os.makedirs(os.path.join(self.proj, d), exist_ok=True)
        os.makedirs(self.home)
        arquivos = {"application/models/M_veiculo.php": MODEL, "vendor/bin/phpunit": PHPUNIT_MINIMO, "compose.yaml": COMPOSE,
                    "Dockerfile": "FROM php:8.3-apache\nENV APP_SECRET=nao-mostrar-isto\nCOPY . /var/www/html\n",
                    ".env": "DB_PASSWORD=segredo-do-env\n"}
        for rel, c in arquivos.items():
            with open(os.path.join(self.proj, rel), "w") as f:
                f.write(c)
        git(self.proj, "init", "-q", "-b", "principal")
        git(self.proj, "add", "-A")
        git(self.proj, "commit", "-q", "-m", "inicial")
        if corromper_git:
            with open(os.path.join(self.proj, ".git", "index"), "wb") as f:
                f.write(b"isto nao e um indice do git")
        self.roteiro = os.path.join(self.dir, "roteiro.json")
        with open(self.roteiro, "w") as f:
            json.dump(roteiro, f)
        env = {"HOME": self.home, "PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LIA_SESSIOND_EXECUTOR": "real",
               "LIA_SESSIOND_PROJETO": self.proj, "LIA_MOCK_CODIGO": self.roteiro}
        for k in ("LIA_SESSIOND_SEM_RLIMIT_PARA_TESTE", "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"):
            if os.environ.get(k):
                env[k] = os.environ[k]
        self.d = subprocess.Popen([SESSIOND], env=env, stdout=open(os.path.join(self.home, "sessiond.log"), "w"),
                                  stderr=subprocess.STDOUT, start_new_session=True)
        sock = os.path.join(self.home, ".lia", "run", "session.sock")
        for _ in range(200):
            if os.path.exists(sock):
                break
            time.sleep(0.05)
        self.c = Conexao(self.home)
        self.msgs = []

    def ate(self, tipo, lim=40):
        msgs, m = self.c.ate(tipo, lim)
        self.msgs += msgs
        return m

    def perguntar(self, texto):
        self.c.enviar("perguntar", {"mensagem": texto})
        return self.ate("proposta")

    def solicitar(self, pr):
        pl = pr["dados"]["plano_proposto"]
        self.c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": pr["dados"]["pergunta_id"]})

    def decidir(self, plano, d="aprovar"):
        self.c.enviar("aprovar", {"plano_id": plano["dados"]["plano_id"], "hash": plano["dados"]["hash"], "decisao": d, "escopo": "uma_vez"})

    def progresso(self):
        return [m["dados"]["mensagem"] for m in self.msgs if m["tipo"] == "progresso" and "codigo" in m["dados"]]

    def apresentacoes(self):
        return "\n".join(m["dados"]["tarefa"]["passo"].get("apresentacao", "") for m in self.msgs
                         if m["tipo"] == "progresso" and m["dados"].get("tarefa", {}).get("passo"))

    def ler(self, rel):
        with open(os.path.join(self.proj, rel)) as f:
            return f.read()

    def auditoria(self):
        d = os.path.join(self.home, ".lia", "humano", "auditoria")
        return "".join(open(os.path.join(d, f)).read() for f in os.listdir(d)) if os.path.isdir(d) else ""

    def fim(self):
        self.c.fechar()
        parar(self.d)


def resposta(r):
    return r["dados"]["resposta_texto"] if r else ""


def main():
    if not os.access(SESSIOND, os.X_OK):
        print("lia-sessiond não encontrado — rode 'make teste-coding-php'")
        return 1
    if not shutil.which("php"):
        print("php não instalado: cenários PHP pulados")
        return 0
    base = tempfile.mkdtemp(prefix="lia-codphp-", dir="/tmp")
    projs = []
    try:
        print("== P1. pedido de código com Git ok: símbolo achado, bind + teste PHP sem banco, validação isolada ==")
        p = Projeto(base, "p1", {"plano": PLANO, "edicoes": [EDICAO]})
        projs.append(p)
        pr = p.perguntar(PEDIDO)
        checar(pr and pr["dados"].get("codigo") is True, "P1: o pedido exato vira tarefa de código (não substituição literal nem git.status)", pr and pr["dados"])
        plano = p.ate("plano")
        prog = p.progresso()
        checar(any("ramo principal, árvore limpa" in x for x in prog), "P1: Git consultado como pré-condição (árvore limpa)", prog)
        checar(any("definidos em: application/models/M_veiculo.php" in x for x in prog) and any(x.startswith("li application/models/M_veiculo.php") for x in prog),
               "P1: o arquivo que define o símbolo é achado por busca e lido", prog)
        prev = "\n".join(x.get("previa", "") for x in (plano["dados"].get("previas") or [])) if plano else ""
        checar("+            where mv.idveiculo = ?" in prev and "+        return $this->db->query($query, array($id))->result();" in prev and
               "+++ b/tests/M_veiculo_getmanutencaoTest.php" in prev and "BancoFalsoLia" in prev and "criar a pasta tests" in prev,
               "P1: prévia com placeholder e argumento de bind separados, e o teste novo com banco falso", prev)
        checar(any("receita determinística de SQL com bind" in x for x in p.progresso()), "P1: proposta pela receita determinística (sem modelo)", p.progresso())
        checar(p.ler("application/models/M_veiculo.php") == MODEL and not os.path.exists(os.path.join(p.proj, "tests/M_veiculoTest.php")),
               "P1: nada gravado antes da aprovação")
        p.decidir(plano)
        v = p.ate("plano")
        pv = (v["dados"].get("previas") or []) if v else []
        textos = [x.get("previa", "") for x in pv]
        checar(len(pv) == 2 and 'argv: ["php","-l","application/models/M_veiculo.php","tests/M_veiculo_getmanutencaoTest.php"]' in textos[0] and
               'argv: ["php","vendor/bin/phpunit","--do-not-cache-result","tests/M_veiculo_getmanutencaoTest.php"]' in textos[1] and "sem rede" in textos[1],
               "P1: validação = php -l dos arquivos tocados + teste direcionado, isolados, numa aprovação", textos)
        p.decidir(v)
        r = p.ate("resposta", 60)
        t = resposta(r)
        checar(t.startswith("Resposta: concluído") and "✓ usa placeholder" in t and "1 teste(s), 0 falha(s)" in t, "P1: concluído com o teste PHP passando sem banco", t)
        checar("Git antes de começar: ramo principal, árvore limpa" in t and "Origem da última proposta: receita determinística" in t,
               "P1: relatório mostra o estado do Git antes e a origem da proposta", t)
        st = git(p.proj, "status", "--porcelain", "-uall")
        checar(sorted(st.split("\n")[:-1]) == sorted([" M application/models/M_veiculo.php", "?? tests/M_veiculo_getmanutencaoTest.php"]),
               "P1: só o arquivo alvo e o teste novo mudaram (sem cache do PHPUnit)", st)
        checar("segredo-do-env" not in json.dumps(p.msgs), "P1: .env nunca foi lido nem enviado")
        p.fim()

        print("== P1b. sem pedido de SQL/bind: a proposta vem do modelo (mock), com o mesmo verificador ==")
        p = Projeto(base, "p1b", {"plano": PLANO, "edicoes": [EDICAO]})
        projs.append(p)
        p.perguntar("Ajuste `M_veiculo::getmanutencao` para passar o id como parâmetro da consulta e crie um teste PHPUnit sem banco.")
        plano = p.ate("plano")
        prev = "\n".join(x.get("previa", "") for x in (plano["dados"].get("previas") or [])) if plano else ""
        checar("+++ b/tests/M_veiculoTest.php" in prev and not any("receita" in x for x in p.progresso()), "P1b: proposta do modelo, prévia normal", prev[:500])
        p.decidir(plano, "rejeitar")
        r = p.ate("resposta")
        checar("Origem da última proposta: modelo local" in resposta(r), "P1b: relatório diz que a proposta veio do modelo", resposta(r))
        p.fim()

        print("== P2. Git quebrado: a pré-condição falha, a Lia explica e segue ==")
        p = Projeto(base, "p2", {"plano": PLANO, "edicoes": [EDICAO]}, corromper_git=True)
        projs.append(p)
        p.perguntar(PEDIDO)
        plano = p.ate("plano")
        prog = p.progresso()
        checar(plano and any("não foi possível ler o estado do Git" in x and "segui sem ele" in x for x in prog),
               "P2: falha do Git explicada e o objetivo continua (proposta chega)", prog)
        p.decidir(plano, "rejeitar")
        r = p.ate("resposta")
        checar("NÃO concluído" in resposta(r) and "Git antes de começar: não foi possível ler o estado do Git" in resposta(r),
               "P2: relatório honesto, com o erro do Git", resposta(r))
        print("== P3. erro de Git numa consulta: reportado, sem falso 'concluído' ==")
        pr = p.perguntar("git status")
        p.solicitar(pr)
        res = p.ate("resultado")
        r = p.ate("resposta")
        checar(res and res["dados"]["estado"] != "sucesso" and resposta(r).startswith("Não foi feito (git.status)") and "concluído" not in resposta(r).lower(),
               "P3: git.status falhando vira 'Não foi feito' com o erro real", resposta(r))
        p.fim()

        print("== P4. pedido que proíbe o Git ==")
        p = Projeto(base, "p4", {"plano": PLANO, "edicoes": [EDICAO]})
        projs.append(p)
        pr = p.perguntar(PEDIDO + " Não rode Git, não use git status.")
        checar(pr and pr["dados"].get("codigo") is True, "P4: continua sendo tarefa de código (não vira git.status)", pr and pr["dados"])
        plano = p.ate("plano")
        p.decidir(plano, "rejeitar")
        r = p.ate("resposta")
        checar(any("o pedido proíbe o Git" in x for x in p.progresso()) and "acao=sessiond.git" not in p.auditoria() and
               "Git antes de começar: não consultado — o pedido proíbe o Git" in resposta(r), "P4: nenhum Git executado; o relatório diz por quê", resposta(r))
        p.fim()

        print("== P5. leitura + pergunta analítica: responde à pergunta com evidência e hipótese ==")
        p = Projeto(base, "p5", {})
        projs.append(p)
        pr = p.perguntar("leia application/models/M_veiculo.php e explique se getmanutencao tem SQL injection")
        p.solicitar(pr)
        p.ate("resultado")
        r = p.ate("resposta", 30)
        t = resposta(r)
        checar(t.startswith("Resposta: (mock) a pergunta foi respondida") and "Evidência (linhas lidas agora):" in t and
               "application/models/M_veiculo.php:1 —" in t, "P5: resposta à pergunta com evidência arquivo:linha conferida", t)
        checar("Achados por regra fixa" in t and "application/models/M_veiculo.php:10 — $id entra direto na SQL" in t,
               "P5: regra fixa aponta a linha exata da interpolação", t)
        checar("Hipóteses (não verificadas" in t and "99999" not in t and "1 referência(s) do modelo descartada(s)" in (r["dados"].get("aviso") or ""),
               "P5: referência a linha não lida é descartada; hipótese separada de evidência", r and r["dados"])
        pr = p.perguntar("leia application/models/M_veiculo.php e explique o problema MOCK_ANALISE_INVALIDA")
        p.solicitar(pr)
        p.ate("resultado")
        r = p.ate("resposta", 30)
        checar("não consegui a análise do modelo" in resposta(r) and "$id entra direto na SQL" in resposta(r),
               "P5: modelo sem JSON -> só a regra fixa, dito claramente", resposta(r))

        print("== P6. teste PHP executado sem banco (validação isolada sem rede) ==")
        pr = p.perguntar("rode os testes")
        p.solicitar(pr)
        pl = p.ate("plano")
        checar(pl and 'argv: ["php","vendor/bin/phpunit","--do-not-cache-result"]' in (pl["dados"].get("previas") or [{}])[0].get("previa", ""),
               "P6: PHPUnit do Composer detectado, sem cache de resultado")
        p.decidir(pl)
        r = p.ate("resposta", 60)
        checar("A validação \"teste\" falhou" in resposta(r), "P6: sem testes no projeto, a validação falha honestamente", resposta(r))

        print("== P7. manifests Docker: serviços explicados, segredos ocultados ==")
        pr = p.perguntar("explique os serviços, portas, volumes e riscos do docker compose deste projeto")
        pl = pr["dados"].get("plano_proposto") or {}
        lidos = [x["parametros"]["caminho"] for x in pl.get("passos", [])]
        checar(sorted(lidos) == ["Dockerfile", "compose.yaml"], "P7: os manifests do projeto são achados sem caminho no pedido", lidos)
        p.solicitar(pr)
        p.ate("resultado")
        r = p.ate("resposta", 30)
        ap = p.apresentacoes()
        checar("services:" in ap and "«omitido»" in ap and "segredo-do-banco-123" not in json.dumps(p.msgs) and "nao-mostrar-isto" not in json.dumps(p.msgs) and "desenvolvimento" not in json.dumps(p.msgs) and "APP_ENV: «omitido»" in ap,
               "P7: valores de segredo do compose e do Dockerfile nunca aparecem", ap[:600])
        checar(resposta(r).startswith("Resposta:") and ("compose.yaml:" in resposta(r) or "Dockerfile:" in resposta(r)), "P7: resposta analítica com referência a um manifest lido", resposta(r))

        print("== P8. arquivo sensível ==")
        pr = p.perguntar("leia o arquivo .env")
        p.solicitar(pr)
        p.ate("resultado")
        r = p.ate("resposta")
        checar("parece conter segredos" in resposta(r) and "segredo-do-env" not in json.dumps(p.msgs), "P8: .env recusado, conteúdo nunca enviado", resposta(r))
        pr = p.perguntar("procure \"DB_PASSWORD\" no projeto")
        p.solicitar(pr)
        p.ate("resultado")
        p.ate("resposta")
        checar("segredo-do-env" not in json.dumps(p.msgs) and ".env:" not in p.apresentacoes(), "P8: busca não entra em arquivo sensível")
        p.fim()
    finally:
        for p in projs:
            parar(p.d)
    restos = [x for x in subprocess.run(["pgrep", "-f", base], capture_output=True, text=True).stdout.split() if x != str(os.getpid())]
    checar(not restos, "nenhum processo residual", restos)
    if not os.environ.get("LIA_DEBUG_MANTER"):
        shutil.rmtree(base, ignore_errors=True)
    print("\n%d testes, %d falhas" % (total, falhas))
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
