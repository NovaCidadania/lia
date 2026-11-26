#!/usr/bin/env python3
"""Testes do harness de avaliação (ferramentas/avaliacao): protocolo, limpeza de processos e runner.

Sem modelo e sem rede. O protocolo é testado contra um daemon FALSO (socket unix) que reproduz a
sequência real do lia-sessiond; o caso curto com o daemon REAL (sem-modelo) roda se build/lia-sessiond
e as fixtures existirem. Uso: make teste-harness-avaliacao"""
import json, os, signal, socket, subprocess, sys, tempfile, threading, time

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(RAIZ, "ferramentas", "avaliacao"))
sys.path.insert(0, os.path.join(RAIZ, "testes", "integration"))
import protocolo  # noqa: E402
from teste_web_resposta import Conexao  # noqa: E402

total = falhas = 0


def checar(cond, desc, detalhe=""):
    global total, falhas
    total += 1
    if cond:
        print("ok   " + desc, flush=True)
    else:
        falhas += 1
        print("FALHA " + desc + (("\n      " + str(detalhe)[:1500]) if detalhe != "" else ""), flush=True)


class DaemonFalso:
    """socket em <home>/.lia/run/session.sock; 'roteiro(srv)' roda numa thread e fala com um cliente"""

    def __init__(self, roteiro):
        self.home = tempfile.mkdtemp(prefix="lia-h-", dir="/tmp")
        os.makedirs(os.path.join(self.home, ".lia", "run"))
        self.sock = os.path.join(self.home, ".lia", "run", "session.sock")
        self.srv = socket.socket(socket.AF_UNIX)
        self.srv.bind(self.sock)
        self.srv.listen(1)
        self.recebidas = []
        self.roteiro = roteiro
        self.t = threading.Thread(target=self._rodar, daemon=True)
        self.t.start()

    def _rodar(self):
        try:
            self.cli, _ = self.srv.accept()
            self.buf = b""
            self.roteiro(self)
        except OSError:
            pass

    def enviar(self, tipo, dados):
        self.cli.sendall((json.dumps({"protocolo": "lia.v1", "tipo": tipo, "id": "d1", "em_resposta_a": None, "ts": "t", "dados": dados}) + "\n").encode())

    def enviar_bruto(self, linha):
        self.cli.sendall((linha + "\n").encode())

    def receber(self, limite=5.0):
        fim = time.time() + limite
        while b"\n" not in self.buf and time.time() < fim:
            self.cli.settimeout(max(0.05, fim - time.time()))
            try:
                d = self.cli.recv(65536)
            except socket.timeout:
                continue
            if not d:
                return None
            self.buf += d
        if b"\n" not in self.buf:
            return None
        linha, self.buf = self.buf.split(b"\n", 1)
        m = json.loads(linha)
        self.recebidas.append(m)
        return m

    def fechar(self):
        for x in (getattr(self, "cli", None), self.srv):
            try:
                x.close()
            except Exception:
                pass


PLANO = {"plano_id": "p1", "hash": "h1", "intencao": "arquivo_alterar"}
politica_aprova = lambda dd: ("aprovar", "", [{"ferramenta": "arquivo.escrever", "caminho": "a.py"}])  # noqa: E731


def politica_real(dd):
    cam = ((dd.get("passos") or [{}])[0].get("parametros") or {}).get("caminho", "")
    if cam.startswith("/") or ".." in cam.split("/"):
        return "rejeitar", "caminho fora do escopo: " + cam, [{"ferramenta": "arquivo.escrever", "caminho": cam}]
    return "aprovar", "", [{"ferramenta": "arquivo.escrever", "caminho": cam}]


def rodar(roteiro, politica=politica_aprova, limite_s=20, sem_progresso_s=3):
    d = DaemonFalso(roteiro)
    c = Conexao(d.home)
    t0 = time.time()
    try:
        out = protocolo.conduzir(c, "faça X", politica, limite_s, sem_progresso_s)
        time.sleep(0.4)  # deixa o daemon falso ler a última mensagem do cliente antes de fechar
    finally:
        c.fechar()
        d.fechar()
    return out, d, time.time() - t0


def teste_protocolo():
    # 1. handshake COMPLETO com plano_proposto (a regressão da auditoria: o avaliador antigo esperava 40 min aqui)
    def completo(d):
        d.receber()  # perguntar
        d.enviar("progresso", {"mensagem": "pensando"})
        d.enviar("proposta", {"pergunta_id": "q1", "tipo_resposta": "tarefa", "plano_proposto": PLANO, "resposta_texto": ""})
        sol = d.receber()
        assert sol and sol["tipo"] == "solicitar"
        d.enviar("plano", {"plano_id": "p1", "hash": "h1", "intencao": "arquivo_alterar", "requer_aprovacao": True,
                           "passos": [{"ferramenta": "arquivo.escrever", "parametros": {"caminho": "a.py"}}]})
        d.receber()  # aprovar
        d.enviar("resultado", {"ok": True})
        d.enviar("resposta", {"pergunta_id": "q1", "resposta_texto": "Resposta: concluído"})
        time.sleep(0.3)
    out, d, dt = rodar(completo, politica_real)
    tipos = [m["tipo"] for m in d.recebidas]
    checar(out["termino"] == "resposta" and out["resposta"] == "Resposta: concluído", "handshake completo: proposta(plano_proposto) -> solicitar -> plano -> aprovar -> resultado -> resposta", out)
    checar(tipos == ["perguntar", "solicitar", "aprovar"], "ordem das mensagens do cliente: perguntar, solicitar, aprovar", tipos)
    sol = d.recebidas[1]["dados"]
    checar(sol["plano_id"] == "p1" and sol["hash"] == "h1" and sol["origem_modelo"] is True and sol["pergunta_id"] == "q1",
           "solicitar preso ao plano_id/hash da proposta, com origem_modelo e pergunta_id", sol)
    apr = d.recebidas[2]["dados"]
    checar(apr["plano_id"] == "p1" and apr["hash"] == "h1" and apr["decisao"] == "aprovar" and apr["escopo"] == "uma_vez", "aprovar preso ao hash do plano", apr)
    checar(len(out["solicitacoes"]) == 1 and len(out["aprovacoes"]) == 1 and out["aprovacoes"][0]["hash"] == "h1", "solicitação e aprovação registradas com o hash", out)

    # 2. plano rejeitado pela política (caminho fora do escopo): decisão 'rejeitar' registrada e o fluxo termina
    def rejeitada(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO})
        d.receber()
        d.enviar("plano", {"plano_id": "p1", "hash": "h1", "intencao": "arquivo_alterar", "passos": [{"ferramenta": "arquivo.escrever", "parametros": {"caminho": "../fora.py"}}]})
        d.receber()
        d.enviar("resposta", {"pergunta_id": "q1", "resposta_texto": "NÃO concluído: plano rejeitado"})
        time.sleep(0.3)
    out, d, dt = rodar(rejeitada, politica_real)
    checar(out["termino"] == "resposta" and out["aprovacoes"][0]["decisao"] == "rejeitar" and "fora do escopo" in out["aprovacoes"][0]["motivo"] and d.recebidas[2]["dados"]["decisao"] == "rejeitar",
           "proposta rejeitada pela política: 'rejeitar' enviado e registrado com o motivo", out)

    # 3. SEM PROGRESSO: o daemon propõe plano, recebe 'solicitar' e fica mudo -> erro de harness em segundos, não 40 min
    def mudo(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO})
        d.receber()
        time.sleep(8)
    out, d, dt = rodar(mudo, sem_progresso_s=2)
    checar(out["termino"] == "sem_progresso" and dt < 8 and "último evento: proposta" in out["diagnostico"], "sem progresso: erro de harness com diagnóstico em poucos segundos", (out["termino"], dt, out["diagnostico"]))

    # 3b. proposta de plano que o cliente ANTIGO ignorava: a conexão fica aberta e muda desde o início
    def mudo2(d):
        d.receber()
        time.sleep(8)
    out, d, dt = rodar(mudo2, sem_progresso_s=2)
    checar(out["termino"] == "sem_progresso" and dt < 8, "daemon mudo desde o início também termina rápido", (out["termino"], dt))

    # 4. limite de tempo com progresso contínuo: NÃO é erro de harness (o modelo/runtime é que não terminou)
    def vivo(d):
        d.receber()
        for _ in range(40):
            d.enviar("progresso", {"mensagem": "ainda"})
            time.sleep(0.3)
    out, d, dt = rodar(vivo, limite_s=2, sem_progresso_s=5)
    checar(out["termino"] == "limite_tempo" and out["termino"] not in protocolo.TERMINOS_DE_HARNESS, "limite de tempo com eventos chegando = limite_tempo (não é defeito de harness)", out["termino"])

    # 5. desconexão
    def cai(d):
        d.receber()
        d.enviar("progresso", {"mensagem": "x"})
        time.sleep(0.2)
        d.cli.close()
    out, d, dt = rodar(cai, sem_progresso_s=30)
    checar(out["termino"] == "desconectado" and dt < 8, "desconexão do daemon: erro terminal com diagnóstico imediato", (out["termino"], dt))

    # 6. evento 'erro' do daemon e tipo desconhecido
    def erro(d):
        d.receber()
        d.enviar("erro", {"codigo": "plano_alterado", "mensagem": "o hash informado não corresponde"})
        time.sleep(0.3)
    out, d, dt = rodar(erro)
    checar(out["termino"] == "erro_protocolo" and "plano_alterado" in out["diagnostico"], "'erro' do daemon é terminal e vai no diagnóstico", out)

    def estranho(d):
        d.receber()
        d.enviar("evento_novo", {})
        time.sleep(0.3)
    out, d, dt = rodar(estranho)
    checar(out["termino"] == "evento_inesperado" and "evento_novo" in out["diagnostico"], "tipo de evento desconhecido é terminal", out)

    # 7. a mesma proposta de plano duas vezes: não entra em laço
    def repete(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO})
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO})
        time.sleep(0.3)
    out, d, dt = rodar(repete)
    checar(out["termino"] == "handshake_repetido", "plano reproposto com o mesmo id: handshake_repetido", out["termino"])

    # 8. 'proposta' sem plano nem codigo = resposta final em texto; com 'codigo' continua esperando
    def so_texto(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": None, "tipo_resposta": "resposta", "resposta_texto": "não consigo"})
        time.sleep(0.3)
    out, d, dt = rodar(so_texto)
    checar(out["termino"] == "resposta" and out["resposta"] == "não consigo", "proposta só com texto é a resposta final", out)

    def coding(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "codigo": {"tarefa": "x"}, "plano_proposto": None})
        d.enviar("progresso", {"mensagem": "lendo"})
        d.enviar("resposta", {"pergunta_id": "q1", "resposta_texto": "Resposta: concluído"})
        time.sleep(0.3)
    out, d, dt = rodar(coding)
    checar(out["termino"] == "resposta" and out["resposta"].startswith("Resposta: concluído") and not d.recebidas[1:], "ciclo de coding: proposta(codigo) não é terminal e não pede solicitar", (out, d.recebidas))


def teste_protocolo_estrito():
    """o avaliador não aprova nada que não tenha solicitado (auditoria 2026-11-26)"""
    # plano SEM solicitação e sem ciclo de coding: rejeita e encerra como erro de harness
    def sem_solicitar(d):
        d.receber()
        d.enviar("plano", {"plano_id": "px", "hash": "hx", "intencao": "arquivo_alterar", "passos": [{"ferramenta": "arquivo.escrever", "parametros": {"caminho": "a.py"}}]})
        d.receber()
        time.sleep(0.3)
    out, d, dt = rodar(sem_solicitar)
    rej = [m for m in d.recebidas if m["tipo"] == "aprovar"]
    checar(out["termino"] == "plano_sem_solicitacao" and rej and rej[0]["dados"]["decisao"] == "rejeitar" and rej[0]["dados"]["hash"] == "hx",
           "plano sem 'solicitar' correspondente: NÃO é aprovado — rejeitado (preso ao hash) e erro de harness", (out["termino"], [m["tipo"] for m in d.recebidas]))

    # plano com hash diferente do solicitado
    def hash_diferente(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO})
        d.receber()
        d.enviar("plano", {"plano_id": "p1", "hash": "OUTRO", "intencao": "arquivo_alterar", "passos": []})
        d.receber()
        time.sleep(0.3)
    out, d, dt = rodar(hash_diferente)
    checar(out["termino"] == "plano_divergente" and "OUTRO" in out["diagnostico"] and d.recebidas[-1]["dados"]["decisao"] == "rejeitar",
           "plano com hash diferente do solicitado: rejeitado, erro de harness", out["diagnostico"])

    # plano com id diferente do solicitado
    def id_diferente(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO})
        d.receber()
        d.enviar("plano", {"plano_id": "p9", "hash": "h1", "intencao": "arquivo_alterar", "passos": []})
        d.receber()
        time.sleep(0.3)
    out, d, dt = rodar(id_diferente)
    checar(out["termino"] == "plano_divergente" and d.recebidas[-1]["dados"]["decisao"] == "rejeitar", "plano com id diferente do solicitado: rejeitado", out["termino"])

    # resposta de OUTRA pergunta
    def outra_pergunta(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO})
        d.receber()
        d.enviar("resposta", {"pergunta_id": "q2", "resposta_texto": "Resposta: concluído"})
        time.sleep(0.3)
    out, d, dt = rodar(outra_pergunta)
    checar(out["termino"] == "pergunta_divergente" and out["resposta"] is None, "resposta de outra pergunta não é aceita como final", out)

    # a resposta da MESMA pergunta é aceita
    def mesma_pergunta(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO})
        d.receber()
        d.enviar("resposta", {"pergunta_id": "q1", "resposta_texto": "ok"})
        time.sleep(0.3)
    out, d, dt = rodar(mesma_pergunta)
    checar(out["termino"] == "resposta" and out["resposta"] == "ok", "resposta da mesma pergunta é aceita", out["termino"])

    # coding: plano de aprovação SEM 'solicitar' é legítimo SÓ depois de proposta(codigo); proposta de coding não é resposta final
    def coding_com_plano(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "tipo_resposta": "codigo", "codigo": True, "codigo_id": "c1", "resposta_texto": "ack"})
        d.enviar("plano", {"plano_id": "pc", "hash": "hc", "intencao": "arquivo_alterar", "passos": [{"ferramenta": "arquivo.escrever", "parametros": {"caminho": "a.py"}}]})
        d.receber()
        d.enviar("resposta", {"pergunta_id": "q1", "resposta_texto": "Resposta: concluído"})
        time.sleep(0.3)
    out, d, dt = rodar(coding_com_plano, politica_real)
    tipos = [m["tipo"] for m in d.recebidas]
    checar(out["termino"] == "resposta" and tipos == ["perguntar", "aprovar"] and out["aprovacoes"][0]["decisao"] == "aprovar",
           "ciclo de coding: proposta(codigo) não é resposta final; plano de gravação é aprovado pela política, sem 'solicitar'", (out["termino"], tipos))

    # proposta de coding com tipo_resposta='codigo' e ainda pendente: o limite de tempo (não 'resposta')
    def coding_pendente(d):
        d.receber()
        d.enviar("proposta", {"pergunta_id": "q1", "tipo_resposta": "codigo", "codigo": True, "resposta_texto": "ack"})
        for _ in range(30):
            d.enviar("progresso", {"pergunta_id": "q1", "mensagem": "validando", "codigo": {"etapa": "validacao"}})
            time.sleep(0.2)
    out, d, dt = rodar(coding_pendente, limite_s=1.5, sem_progresso_s=5)
    checar(out["termino"] == "limite_tempo" and out["resposta"] is None, "coding pendente com progresso nunca vira resposta final", out["termino"])


def teste_casos_limite():
    """plano malformado, campos ausentes/nulos, mensagem que não é objeto, aprovação com hash diferente,
    resposta depois da rejeição (auditoria 2026-11-26, revisão final)"""
    def caso(rot):
        return rodar(rot)[0]

    def plano_sem_hash(d):
        d.receber(); d.enviar("proposta", {"pergunta_id": "q1", "codigo": True, "tipo_resposta": "codigo"})
        d.enviar("plano", {"plano_id": "c1", "intencao": "arquivo_alterar"}); time.sleep(0.3)
    out, d, _ = rodar(plano_sem_hash)
    checar(out["termino"] == "mensagem_malformada" and not [m for m in d.recebidas if m["tipo"] == "aprovar"],
           "plano sem hash nunca é aprovado (nem com coding ativo) e não derruba o avaliador", out["termino"])

    def plano_id_nulo(d):
        d.receber(); d.enviar("plano", {"plano_id": None, "hash": None}); time.sleep(0.3)
    checar(caso(plano_id_nulo)["termino"] == "mensagem_malformada", "plano_id/hash nulos: erro de harness", None)

    def proposta_plano_malformado(d):
        d.receber(); d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": {"intencao": "x"}}); time.sleep(0.3)
    out, d, _ = rodar(proposta_plano_malformado)
    checar(out["termino"] == "mensagem_malformada" and not [m for m in d.recebidas if m["tipo"] == "solicitar"],
           "plano_proposto sem id/hash: nenhum 'solicitar' é enviado", out["termino"])

    def pergunta_nula(d):
        d.receber(); d.enviar("proposta", {"pergunta_id": None, "plano_proposto": PLANO}); time.sleep(0.3)
    out, d, _ = rodar(pergunta_nula)
    checar(out["termino"] == "pergunta_divergente" and not [m for m in d.recebidas if m["tipo"] == "solicitar"],
           "proposta com pergunta_id nulo não é vinculável: nada é solicitado", out["termino"])

    def resposta_sem_pergunta(d):
        d.receber(); d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": None, "tipo_resposta": "resposta", "resposta_texto": "x"})
        time.sleep(0.3)
    checar(caso(resposta_sem_pergunta)["termino"] == "resposta", "resposta final textual com pergunta_id continua aceita", None)

    def resposta_pid_ausente(d):
        d.receber(); d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO}); d.receber()
        d.enviar("resposta", {"resposta_texto": "Resposta: concluído"}); time.sleep(0.3)
    out = caso(resposta_pid_ausente)
    checar(out["termino"] == "pergunta_divergente" and out["resposta"] is None, "resposta sem pergunta_id não vale como final", out["termino"])

    def nao_objeto(d):
        d.receber(); d.enviar_bruto("[1,2,3]"); time.sleep(0.3)
    checar(caso(nao_objeto)["termino"] == "mensagem_malformada", "linha JSON que não é objeto: erro de harness", None)

    def dados_lista(d):
        d.receber(); d.enviar_bruto(json.dumps({"tipo": "progresso", "dados": [1]})); time.sleep(0.3)
    checar(caso(dados_lista)["termino"] == "mensagem_malformada", "'dados' que não é objeto: erro de harness", None)

    def hash_diferente(d):
        d.receber(); d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO}); d.receber()
        d.enviar("plano", {"plano_id": "p1", "hash": "OUTRO", "intencao": "arquivo_alterar"})
        d.receber(); d.enviar("resposta", {"pergunta_id": "q1", "resposta_texto": "Resposta: concluído"}); time.sleep(0.3)
    out, d, _ = rodar(hash_diferente)
    ap = [m for m in d.recebidas if m["tipo"] == "aprovar"]
    checar(out["termino"] == "plano_divergente" and out["resposta"] is None and ap and ap[0]["dados"]["decisao"] == "rejeitar"
           and ap[0]["dados"]["hash"] == "OUTRO",
           "hash diferente do solicitado: rejeitado e a 'resposta' que vem depois NÃO é aceita", (out["termino"], out["resposta"]))

    def apos_rejeicao_da_politica(d):
        d.receber(); d.enviar("proposta", {"pergunta_id": "q1", "plano_proposto": PLANO}); d.receber()
        d.enviar("plano", {"plano_id": "p1", "hash": "h1", "intencao": "arquivo_alterar",
                           "passos": [{"ferramenta": "arquivo.escrever", "parametros": {"caminho": "/etc/passwd"}}]})
        d.receber(); d.enviar("resposta", {"pergunta_id": "q1", "resposta_texto": "recusado"}); time.sleep(0.3)
    out, d, _ = rodar(apos_rejeicao_da_politica, politica=politica_real)
    ap = [m for m in d.recebidas if m["tipo"] == "aprovar"]
    checar(out["termino"] == "resposta" and ap and ap[0]["dados"]["decisao"] == "rejeitar",
           "rejeição pela política do avaliador, seguida da resposta do daemon da mesma pergunta, termina normalmente", out["termino"])


def teste_auditoria_diretorio():
    """rastreabilidade: um diretório com versões diferentes ou JSON fora do manifesto não vira 'uma matriz'"""
    import tempfile
    import auditar_diretorio as ad
    with tempfile.TemporaryDirectory() as d:
        def res(nome, sha, cond):
            json.dump({"avaliador_versao": "avaliar/4", "avaliador_sha256": sha, "lia_sessiond_sha256": "b1", "classe": "nao_concluido",
                       "atribuicao": "modelo", "condicao": cond, "tarefa": {"id": "t1"}}, open(os.path.join(d, nome + ".json"), "w"))
        res("a", "v1", "sem-modelo"); res("b", "v2", "ollama:x")
        json.dump({"validas": [{"rotulo": "b"}], "esperadas": 1, "comparavel": True}, open(os.path.join(d, "manifesto.json"), "w"))
        a = ad.auditar(d)
        checar(a["misto"] and not a["comparavel"] and not a["cobertura_confere_com_manifesto"] and a["resultados"] == 2,
               "auditoria: versões mistas e JSON fora do manifesto => misto, não comparável, cobertura não confere", a["avisos"])
        antes = open(os.path.join(d, "a.json"), "rb").read()
        ad.auditar(d)
        checar(open(os.path.join(d, "a.json"), "rb").read() == antes, "auditoria não altera resultados", None)


def teste_atribuicao():
    import avaliar
    a = avaliar.atribuir
    checar(a({"classe": "sucesso"}) == "sucesso" and a({"classe": "erro_de_harness"}) == "harness" and a({"classe": "bloqueado por ambiente"}) == "ambiente"
           and a({"classe": "sem_oraculo"}) == "sem_oraculo", "atribuição: sucesso, harness, ambiente e sem oráculo nunca viram falha do modelo")
    checar(a({"classe": "nao_concluido", "causa": "proposta_invalida"}) == "modelo" and a({"classe": "nao_concluido", "causa": "modelo_timeout"}) == "backend"
           and a({"classe": "concluido_falso", "causa": "concluido", "oculta": {"codigo_saida": 1}}) == "runtime"
           and a({"classe": "nao_concluido", "causa": "validacao", "suite_antes": 1}) == "ambiente"
           and a({"classe": "nao_concluido", "causa": "validacao", "oculta": {"codigo_saida": 127}}) == "oraculo_ou_ambiente",
           "atribuição: modelo, backend, runtime (concluído falso), suíte-base quebrada e oráculo que não rodou")


def teste_politica_de_aprovacao():
    import avaliar
    pol = avaliar.politica_de_plano({"comandos": {"teste": ["python3", "-m", "unittest"]}}, json.dumps(["python3", "-m", "unittest"], separators=(",", ":")))
    def plano(ferr, cam, intencao="arquivo_alterar"):
        return {"intencao": intencao, "passos": [{"ferramenta": ferr, "parametros": {"caminho": cam}}]}
    checar(pol(plano("arquivo.escrever", "inventario/estoque.py"))[0] == "aprovar", "política: gravar código do projeto é aprovado")
    dec, mot, _ = pol(plano("arquivo.apagar", "inventario/estoque.py"))
    checar(dec == "rejeitar" and "destrutiva" in mot, "política: apagar arquivo NÃO é aprovado numa tarefa de coding (smoke 2026-11-26)", (dec, mot))
    checar(pol(plano("arquivo.mover", "a.py"))[0] == "rejeitar" and pol(plano("arquivo.escrever", "../fora.py"))[0] == "rejeitar"
           and pol(plano("arquivo.escrever", ".lia/x"))[0] == "rejeitar" and pol(plano("arquivo.escrever", "/etc/x"))[0] == "rejeitar",
           "política: mover, '..', .lia e caminho absoluto são rejeitados")


def teste_limpeza():
    # daemon + filho + neto em sessão própria (como o lia-sessiond e seus sandboxes)
    p = subprocess.Popen(["sh", "-c", "setsid sleep 300 & sleep 300 & wait"], start_new_session=True)
    time.sleep(0.6)
    desc = protocolo.descendentes(p.pid)
    checar(len(desc) >= 2, "descendentes encontrados (inclusive os de sessão própria)", desc)
    t0 = time.time()
    protocolo.encerrar_arvore(p)
    vivos = [x for x in [p.pid] + desc if protocolo._vivo(x)]
    checar(not vivos and time.time() - t0 < 10, "encerrar_arvore mata o processo e toda a descendência", vivos)
    checar(protocolo.encerrar_arvore(p) in (True, False), "encerrar_arvore é idempotente (processo já morto)")


AVALIADOR_FALSO = r'''#!/usr/bin/env python3
import hashlib, json, os, subprocess, sys, time
VER = {"avaliador_versao": "falso/1", "avaliador_sha256": "a" * 64, "formato": "lia-avaliacao-execucao/3", "lia_sessiond_sha256": "b" * 64}
if sys.argv[1:] == ["--versao"]:
    print(json.dumps(VER)); sys.exit(0)
a = dict(zip(sys.argv[1::2], sys.argv[2::2]))
tid, cond, rep, saida = a["--id"], a["--condicao"], int(a["--rep"]), a["--saida"]
modo = open(os.path.join(saida, "MODO")).read().strip() if os.path.exists(os.path.join(saida, "MODO")) else "ok"
rot = "%s__%s__r%d%s" % (tid, cond.replace(":", "_").replace("/", "_"), rep, ("_T" + a["--temperatura"]) if a.get("--temperatura") else "")
if modo.startswith("harness:") and tid == modo.split(":")[1]:
    print("erro de harness simulado"); sys.exit(3)
if modo.startswith("trava:") and tid == modo.split(":")[1]:
    neto = subprocess.Popen(["setsid", "sleep", "300"])
    open(os.path.join(saida, "NETO"), "w").write(str(neto.pid))
    time.sleep(300)
if modo.startswith("lixo:") and tid == modo.split(":")[1]:
    open(os.path.join(saida, rot + ".json"), "w").write("{nao é json"); sys.exit(0)
res = dict(VER, tarefa={"id": tid}, tarefas_sha256=hashlib.sha256(open(a["--tarefas"], "rb").read()).hexdigest(), condicao=cond, rep=rep,
           modelo_hash=a.get("--modelo-hash", ""), parametros={"LIA_MODELO_TEMPERATURA": a.get("--temperatura", ""), "LIA_MODELO_SEMENTE": a.get("--semente", "")},
           commit_base="abc", estado_terminal="ok", classe="nao_concluido", sucesso_estrito=False, duracao_s=0.1)
json.dump(res, open(os.path.join(saida, rot + ".json"), "w"))
print(rot + ": ok")
'''


def teste_runner():
    tmp = tempfile.mkdtemp(prefix="lia-m-", dir="/tmp")
    fake = os.path.join(tmp, "avaliador_falso.py")
    open(fake, "w").write(AVALIADOR_FALSO)
    tarefas = os.path.join(tmp, "tarefas.json")
    json.dump({"tarefas": [{"id": "t1"}, {"id": "t2"}]}, open(tarefas, "w"))
    runner = [sys.executable, "-I", os.path.join(RAIZ, "ferramentas", "avaliacao", "matriz.py"), "--tarefas", tarefas, "--avaliador", fake,
              "--condicao", "sem-modelo", "--condicao", "ollama:m=hashm", "--sementes", "1", "2", "--temperatura", "0.4"]

    def rodar_runner(saida, extra=()):
        r = subprocess.run(runner + ["--saida", saida] + list(extra), capture_output=True, text=True, timeout=60)
        man = json.load(open(os.path.join(saida, "manifesto.json")))
        return r, man

    # a) tudo ok -> FIM, manifesto completo, 8 combinações
    s1 = os.path.join(tmp, "s1")
    r, man = rodar_runner(s1)
    checar(r.returncode == 0 and "FIM" in r.stdout and man["completo"] and len(man["validas"]) == 8 == man["esperadas"] and not man["faltantes"],
           "runner: todas as combinações válidas -> FIM e manifesto completo", (r.returncode, r.stdout[-300:], man["faltantes"]))
    # b) retomada: pula o que confere
    r, man = rodar_runner(s1)
    checar(r.returncode == 0 and r.stdout.count("pulado") == 8 and len(man["reaproveitadas"]) == 8, "runner: retomada pula só resultados válidos de mesmos metadados", r.stdout[-300:])
    # c) metadado diferente (outro hash de modelo) -> não reaproveita
    f = os.path.join(s1, "t1__ollama_m__r1_T0.4.json")
    d = json.load(open(f))
    d["modelo_hash"] = "outro"
    json.dump(d, open(f, "w"))
    r, man = rodar_runner(s1)
    checar(r.returncode == 0 and "NÃO confere" in r.stdout and r.stdout.count("pulado") == 7 and os.path.exists(f + ".descartado"),
           "runner: resultado com metadado diferente é descartado e refeito (nunca reaproveitado)", r.stdout[-400:])
    # d) JSON truncado deixado por uma queda
    open(f, "w").write("{trunc")
    r, man = rodar_runner(s1)
    checar(r.returncode == 0 and "ilegível" in r.stdout and man["completo"], "runner: JSON ilegível é refeito", r.stdout[-300:])
    # e) falha de harness (exit 3): por PADRÃO é fail-fast — aborta na primeira falha, manifesto preservado, exit 2, sem FIM
    s2 = os.path.join(tmp, "s2")
    os.makedirs(s2)
    open(os.path.join(s2, "MODO"), "w").write("harness:t2")
    r, man = rodar_runner(s2)
    checar(r.returncode == 2 and "ABORTADO" in r.stdout and not any(l.startswith("FIM") for l in r.stdout.splitlines()) and man["abortado_por_harness"]
           and not man["completo"] and not man["comparavel"] and len(man["falhas_de_harness"]) == 1 and len(man["validas"]) == 1 and man["modo"] == "padrao",
           "runner: erro de harness aborta na PRIMEIRA falha (fail-fast), exit 2, manifesto preservado, sem FIM, não comparável",
           (r.returncode, r.stdout[-300:], man["falhas_de_harness"], len(man["validas"])))
    checar("ollama" not in r.stdout and "r2_T" not in r.stdout, "runner: depois da falha NENHUMA outra combinação foi executada", r.stdout)
    # e2) modo diagnóstico opt-in: continua, mas nunca FIM nem comparável
    s2b = os.path.join(tmp, "s2b")
    os.makedirs(s2b)
    open(os.path.join(s2b, "MODO"), "w").write("harness:t2")
    r, man = rodar_runner(s2b, ["--continuar-apos-falha"])
    checar(r.returncode == 1 and "DIAGNOSTICO-INCOMPLETO" in r.stdout and not any(l.startswith("FIM") for l in r.stdout.splitlines()) and man["modo"] == "diagnostico"
           and not man["comparavel"] and not man["completo"] and len(man["falhas_de_harness"]) == 4 and len(man["faltantes"]) == 4 and len(man["validas"]) == 4,
           "runner: --continuar-apos-falha (diagnóstico) continua, mas nunca imprime FIM e o manifesto sai não comparável", (r.returncode, r.stdout[-300:]))
    # e3) o modo diagnóstico mesmo SEM falhas nunca declara FIM nem comparável
    s2c = os.path.join(tmp, "s2c")
    r, man = rodar_runner(s2c, ["--continuar-apos-falha"])
    checar(r.returncode == 1 and not any(l.startswith("FIM") for l in r.stdout.splitlines()) and not man["comparavel"] and len(man["validas"]) == 8,
           "runner: modo diagnóstico sem nenhuma falha ainda assim nunca é FIM/comparável", (r.returncode, r.stdout[-200:]))
    # e4) arquivos .harness-erro/.invalido/.descartado nunca contam como resultado de modelo
    sobra = [f for f in os.listdir(s2b) if f.endswith((".harness-erro.json", ".invalido", ".descartado"))]
    contados = {x["rotulo"] for x in man["validas"]}
    man2 = json.load(open(os.path.join(s2b, "manifesto.json")))
    ok_cont = all(not x["rotulo"].startswith("t2__") for x in man2["validas"])
    open(os.path.join(s1, "t1__sem-modelo__r1_T0.4.json.invalido"), "w").write("{}")
    open(os.path.join(s1, "t1__sem-modelo__r1_T0.4.harness-erro.json"), "w").write("{}")
    open(os.path.join(s1, "t1__sem-modelo__r1_T0.4.json.descartado"), "w").write("{}")
    r, man3 = rodar_runner(s1)
    checar(ok_cont and len(man3["validas"]) == 8 and man3["completo"], "runner: .harness-erro.json, .invalido e .descartado jamais são contados como resultado", (man2["validas"], man3["validas"]))
    # f) avaliador sai 0 mas deixa JSON inválido
    s3 = os.path.join(tmp, "s3")
    os.makedirs(s3)
    open(os.path.join(s3, "MODO"), "w").write("lixo:t1")
    r, man = rodar_runner(s3)
    checar(r.returncode == 2 and not any(l.startswith("FIM") for l in r.stdout.splitlines()) and len(man["falhas_de_harness"]) == 1 and man["abortado_por_harness"],
           "runner: JSON de resultado inválido não vira resultado e aborta (fail-fast)", r.stdout[-300:])
    # g) INTERRUPÇÃO (SIGTERM e SIGINT): sinal no runner enquanto o avaliador (e um neto em sessão própria) trava -> tudo morre, sem FIM
    for sig in (signal.SIGTERM, signal.SIGINT):
      s4 = os.path.join(tmp, "s4_%d" % sig)
      os.makedirs(s4)
      open(os.path.join(s4, "MODO"), "w").write("trava:t1")
      p = subprocess.Popen(runner + ["--saida", s4], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
      neto_f = os.path.join(s4, "NETO")
      for _ in range(100):
          if os.path.exists(neto_f) and open(neto_f).read().strip():
              break
          time.sleep(0.1)
      neto = int(open(neto_f).read())
      checar(protocolo._vivo(neto), "(neto em sessão própria está vivo antes da interrupção)")
      p.send_signal(sig)
      try:
          saida, _ = p.communicate(timeout=30)
      except subprocess.TimeoutExpired:
          p.kill()
          saida = "(runner não encerrou)"
      man = json.load(open(os.path.join(s4, "manifesto.json")))
      checar(p.returncode == 130 and "INTERROMPIDO" in saida and "FIM" not in saida and man["interrompido"] and not man["completo"],
             "runner interrompido por sinal %d: sai 130" % sig + ", manifesto 'interrompido', sem FIM", (p.returncode, saida[-300:]))
      time.sleep(0.5)
      checar(not protocolo._vivo(neto), "runner interrompido (sinal %d): o neto em sessão própria também foi encerrado (sem processo órfão)" % sig, neto)


def teste_caso_curto_real():
    """o caso que antes esperava 40 minutos (ind-py-2, sem-modelo) com o daemon REAL: termina em segundos"""
    sessiond = os.path.join(RAIZ, "build", "lia-sessiond")
    fix = os.path.expanduser("~/.lia/avaliacao-fixtures/inventario-py")
    oculta = os.path.expanduser("~/.lia/avaliacao-oculta/2026-11-25-independente-v1.json")
    tarefas = os.path.join(RAIZ, "docs", "avaliacao", "2026-11-25-independente-v1.json")
    if not (os.path.exists(sessiond) and os.path.isdir(fix) and os.path.exists(oculta) and os.path.exists(tarefas)):
        print("PULADO caso curto real (falta build/lia-sessiond, fixtures ou oráculo)")
        return
    saida = tempfile.mkdtemp(prefix="lia-curto-", dir="/tmp")
    t0 = time.time()
    r = subprocess.run([sys.executable, "-I", os.path.join(RAIZ, "ferramentas", "avaliacao", "avaliar.py"), "--tarefas", tarefas, "--id", "ind-py-2",
                        "--condicao", "sem-modelo", "--rep", "1", "--saida", saida, "--limite-min", "2", "--sem-progresso-s", "45"],
                       capture_output=True, text=True, timeout=180)
    dt = time.time() - t0
    arqs = [f for f in os.listdir(saida) if f.endswith(".json")]
    res = json.load(open(os.path.join(saida, arqs[0]))) if arqs else {}
    checar(dt < 120 and r.returncode in (0, 3) and arqs, "caso curto real (ind-py-2, sem-modelo): termina em %.0f s, não em 40 min" % dt, (r.returncode, r.stdout[-300:], r.stderr[-300:]))
    checar(res.get("termino_do_protocolo") == "resposta" and res.get("estado_terminal") == "ok" and res.get("classe") == "nao_concluido",
           "caso curto real: o handshake percorreu proposta(plano_proposto) -> solicitar e terminou com resposta do daemon",
           (res.get("termino_do_protocolo"), res.get("classe"), res.get("harness_erro"), [e["tipo"] for e in res.get("eventos", [])][:8]))
    print("      eventos: %s | solicitações: %d | aprovações: %s" % ([e["tipo"] for e in res.get("eventos", [])], len(res.get("solicitacoes", [])),
                                                                 [x["decisao"] for x in res.get("aprovacoes", [])]))
    orfaos = subprocess.run(["pgrep", "-f", saida], capture_output=True, text=True).stdout.split()
    checar(not [x for x in orfaos if x != str(os.getpid())], "caso curto real: nenhum processo do daemon ficou para trás", orfaos)


def teste_retencao():
    """retenção opt-in de rejeitadas: só fixture sintética, 0600/0700, sem código real; desligada por padrão"""
    import stat
    sys.path.insert(0, os.path.join(RAIZ, "ferramentas", "avaliacao"))
    import avaliar
    checar(avaliar.e_fixture_sintetica("inventario-py") and avaliar.e_fixture_sintetica("agenda-js") and not avaliar.e_fixture_sintetica("sf")
           and not avaliar.e_fixture_sintetica("le") and not avaliar.e_fixture_sintetica("sweduc"), "retenção: só as fixtures sintéticas declaradas são elegíveis")
    sessiond = os.path.join(RAIZ, "build", "lia-sessiond")
    oculta = os.path.expanduser("~/.lia/avaliacao-oculta/2026-11-25-independente-v1.json")
    tarefas = os.path.join(RAIZ, "docs", "avaliacao", "2026-11-25-independente-v1.json")
    if not (os.path.exists(sessiond) and os.path.exists(oculta) and os.path.isdir(os.path.expanduser("~/.lia/avaliacao-fixtures/inventario-py"))):
        print("PULADO retenção com daemon real")
        return
    for reter in (False, True):
        saida = tempfile.mkdtemp(prefix="lia-ret-", dir="/tmp")
        cmd = [sys.executable, "-I", os.path.join(RAIZ, "ferramentas", "avaliacao", "avaliar.py"), "--tarefas", tarefas, "--id", "ind-py-1",
               "--condicao", "sem-modelo", "--rep", "1", "--saida", saida, "--limite-min", "2", "--sem-progresso-s", "45"]
        subprocess.run(cmd + (["--reter-rejeitadas"] if reter else []), capture_output=True, text=True, timeout=180)
        arqs = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(saida, "rejeitadas")) for f in fs]
        if not reter:
            checar(not arqs, "retenção desligada por padrão: nada é gravado", arqs)
            continue
        checar(len(arqs) == 1, "retenção ligada (fixture sintética): um arquivo de rejeitadas", arqs)
        if arqs:
            modo_arq = stat.S_IMODE(os.stat(arqs[0]).st_mode)
            modo_dir = stat.S_IMODE(os.stat(os.path.dirname(arqs[0])).st_mode)
            linhas = [json.loads(l) for l in open(arqs[0]) if l.strip()]
            checar(modo_arq == 0o600 and modo_dir == 0o700, "retenção: arquivo 0600 e diretório 0700", (oct(modo_arq), oct(modo_dir)))
            checar(linhas and all({"formato", "tarefa", "ciclo", "contexto_sha256", "resposta", "motivo_da_recusa", "validacao"} <= set(l) for l in linhas)
                   and linhas[0]["formato"] == "lia-proposta-rejeitada/1", "retenção: contexto-hash, resposta, motivo e validação por linha", linhas[:1])


if __name__ == "__main__":
    for f in (teste_protocolo, teste_protocolo_estrito, teste_casos_limite, teste_auditoria_diretorio, teste_atribuicao, teste_politica_de_aprovacao, teste_limpeza, teste_runner, teste_caso_curto_real, teste_retencao):
        print("== " + f.__name__, flush=True)
        f()
    print("\n%d testes, %d falhas" % (total, falhas))
    sys.exit(1 if falhas else 0)
