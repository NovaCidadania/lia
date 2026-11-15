#!/usr/bin/env python3
"""Testes de integração do Agent Runtime v2 (docs/protocolo.md §11) no nível
do protocolo: plano de vários passos, validação integral, execução paralela,
dependências, falha, replanejamento limitado, orçamento, cancelamento global,
aprovação vinculada ao hash. Determinísticos: modelo mock; executor mock
para tempo/aprovação e executor real para dados de verdade.

Sempre em HOME temporário sob /tmp. Uso: make teste-modelo"""
import json, os, re, socket, subprocess, sys, tempfile, time

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
        print("FALHA " + desc + (("\n      " + str(detalhe)[:1500]) if detalhe != "" else ""), flush=True)


def iniciar_daemon(home, **env_extra):
    env = {"HOME": home, "PATH": os.environ.get("PATH", "/usr/bin:/bin")}
    for k in ("LIA_SESSIOND_SEM_RLIMIT_PARA_TESTE", "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"):
        if os.environ.get(k):
            env[k] = os.environ[k]
    env.update(env_extra)
    log = open(os.path.join(home, "sessiond.log"), "w")
    p = subprocess.Popen([SESSIOND], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    sock = os.path.join(home, ".lia", "run", "session.sock")
    for _ in range(200):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    return p


def parar(p):
    if p and p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=8)
        except subprocess.TimeoutExpired:
            p.kill()


class Conexao:
    def __init__(self, home):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(os.path.join(home, ".lia", "run", "session.sock"))
        self.buf = b""
        self.n = 0
        self.t0 = time.time()

    def enviar(self, tipo, dados):
        self.n += 1
        ident = "t%d-%d" % (id(self) % 100000, self.n)
        self.s.sendall((json.dumps({"protocolo": "lia.v1", "tipo": tipo, "id": ident, "em_resposta_a": None,
                                    "ts": "t", "dados": dados}) + "\n").encode())
        return ident

    def ler_uma(self, limite):
        fim = time.time() + limite
        while b"\n" not in self.buf:
            resto = fim - time.time()
            if resto <= 0:
                return None
            self.s.settimeout(resto)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                return None
            if not d:
                return None
            self.buf += d
        linha, self.buf = self.buf.split(b"\n", 1)
        m = json.loads(linha)
        m["_t"] = time.time()
        return m

    def ate(self, tipo, limite=15.0):
        """lê até a primeira mensagem do tipo dado; devolve (todas, a_ultima_ou_None)"""
        todas = []
        fim = time.time() + limite
        while time.time() < fim:
            m = self.ler_uma(fim - time.time())
            if m is None:
                break
            todas.append(m)
            if m["tipo"] == tipo:
                return todas, m
        return todas, None

    def silencio(self, segundos):
        m = self.ler_uma(segundos)
        return m is None, m

    def fechar(self):
        self.s.close()


def passos_por_estado(msgs, estado):
    """índices (1..n) dos passos, na ordem em que o daemon os reportou nesse estado"""
    return [m["dados"]["tarefa"]["passo"]["indice"] for m in msgs
            if m["tipo"] == "progresso" and m["dados"].get("tarefa", {}).get("passo", {}).get("estado") == estado]


def momento(msgs, indice, estado):
    for m in msgs:
        ps = m["dados"].get("tarefa", {}).get("passo") if m["tipo"] == "progresso" else None
        if ps and ps["indice"] == indice and ps["estado"] == estado:
            return m["_t"]
    return None


def plano(passos, **extra):
    d = {"objetivo": extra.pop("objetivo", "teste"), "passos": passos}
    d.update(extra)
    return d


def P(ferramenta, **k):
    d = {"ferramenta": ferramenta, "parametros": k.pop("parametros", {})}
    d.update(k)
    return d


def main():
    if not os.access(SESSIOND, os.X_OK):
        print("lia-sessiond não encontrado — rode 'make teste-modelo'")
        return 1
    base = tempfile.mkdtemp(prefix="lia-tarefa-", dir="/tmp")
    home_real = os.path.expanduser("~")
    assert base.startswith("/tmp/") and base != home_real
    hm, hr = os.path.join(base, "mock"), os.path.join(base, "real")
    os.makedirs(hm)
    os.makedirs(hr)
    print("ambiente isolado: %s (HOME real intocado: %s)" % (base, home_real))
    marca = time.time()
    dm = dr = None
    try:
        # ------------------------------------------------ executor mock
        dm = iniciar_daemon(hm, LIA_SESSIOND_EXECUTOR="mock")

        print("== T1. plano de UM passo ==")
        c = Conexao(hm)
        c.enviar("solicitar", plano([P("mock.leitura")]))
        msgs, pl = c.ate("plano")
        d = pl["dados"] if pl else {}
        checar(pl and d.get("tarefa") is True and len(d["passos"]) == 1 and len(d["hash"]) == 64 and d["requer_aprovacao"] is False and
               d["orcamento_passos"] == 8 and d["orcamento_tempo_ms"] == 60000,
               "T1: 'plano' de tarefa com 1 passo, hash SHA-256, orçamentos (8 passos, 60 s), sem aprovação", d)
        msgs, res = c.ate("resultado")
        checar(res and res["dados"]["estado"] == "sucesso" and res["dados"]["dados"]["veredito"] == "sucesso", "T1: tarefa de 1 passo conclui com veredito sucesso", res)
        checar(sum(1 for m in msgs if m["tipo"] == "auditoria") == 1, "T1: um registro de auditoria por passo")
        _, resp = c.ate("resposta")
        checar(resp and "Conclusão:" in resp["dados"]["resposta_texto"] and "Ações realizadas" in resp["dados"]["resposta_texto"], "T1: resposta final com as seções do relatório", resp)
        aud = open(os.path.join(hm, ".lia", "humano", "auditoria", time.strftime("%Y-%m-%d", time.gmtime()) + ".log")).read()
        checar("acao=sessiond.mock.leitura" in aud and "tarefa_passo=passo-1" in aud, "T1: auditoria em disco registra o passo")
        c.fechar()

        print("== T2. quatro passos independentes rodam em PARALELO ==")
        c = Conexao(hm)
        t0 = time.time()
        c.enviar("solicitar", plano([P("mock.leitura") for _ in range(4)]))
        msgs, res = c.ate("resultado")
        dur = time.time() - t0
        ini = [momento(msgs, i, "executando") for i in (1, 2, 3, 4)]
        prim_fim = min(momento(msgs, i, "sucesso") for i in (1, 2, 3, 4))
        checar(res and res["dados"]["estado"] == "sucesso" and all(ini) and max(ini) < prim_fim,
               "T2: os 4 passos de leitura começam antes de qualquer um terminar (execução paralela)")
        checar(dur < 1.6, "T2: 4 passos de ~0,5 s terminam em %.2f s (em série levariam ~2 s)" % dur)
        checar(len(passos_por_estado(msgs, "executando")) == 4 and len(passos_por_estado(msgs, "sucesso")) == 4, "T2: cada passo inicia e conclui exatamente uma vez (sem execução duplicada)")
        c.ate("resposta")
        c.fechar()

        print("== T3. dependências: cada passo só começa depois do pré-requisito ==")
        c = Conexao(hm)
        t0 = time.time()
        c.enviar("solicitar", plano([P("mock.leitura", id="a"), P("mock.leitura", id="b", depende_de=["a"]),
                                      P("mock.leitura", id="c", depende_de=["b"])]))
        msgs, res = c.ate("resultado")
        dur = time.time() - t0
        ordem_ok = momento(msgs, 1, "sucesso") <= momento(msgs, 2, "executando") and momento(msgs, 2, "sucesso") <= momento(msgs, 3, "executando")
        checar(res and res["dados"]["estado"] == "sucesso" and ordem_ok and dur > 1.3, "T3: a → b → c executam em sequência (%.2f s)" % dur)
        c.ate("resposta")
        c.fechar()

        print("== T4. falha no passo 2: dependente é pulado, replanejamento limitado, resultado INCOMPLETO ==")
        c = Conexao(hm)
        t0 = time.time()
        c.enviar("solicitar", plano([P("mock.leitura"), P("mock.saida_grande"), P("mock.leitura", depende_de=["passo-2"])]))
        msgs, res = c.ate("resultado", 20)
        d = res["dados"] if res else {}
        tar = d.get("dados", {}).get("tarefa", {})
        estados = [p["estado"] for p in tar.get("passos", [])]
        checar(res and d["estado"] == "incompleto" and d["dados"]["veredito"] == "incompleto", "T4: estado 'incompleto' — nunca 'sucesso' com evidência faltando", d.get("estado"))
        checar(estados[:3] == ["sucesso", "falha", "pulado"], "T4: passo 1 sucesso, passo 2 falha, passo 3 (dependente) pulado", estados)
        checar("resultado:mock.saida_grande" in d["dados"]["evidencias_ausentes"], "T4: a evidência ausente é a do passo que falhou")
        checar(1 <= tar["replanejamentos"] <= 3 and len(tar["passos"]) <= 8 and any(p["de_replanejamento"] for p in tar["passos"]),
               "T4: houve replanejamento (nova tentativa), dentro dos limites (%d ciclo(s), %d passos)" % (tar.get("replanejamentos", -1), len(tar.get("passos", []))))
        checar(sum(1 for p in tar["passos"] if p["ferramenta"] == "mock.saida_grande") <= 2, "T4: a mesma ferramenta+parâmetros não é repetida além de 2 vezes")
        checar(any(m["tipo"] == "progresso" and m["dados"]["tarefa"]["fase"] == "replanejando" and m["dados"]["tarefa"].get("plano") for m in msgs),
               "T4: o cliente é avisado do replanejamento, com o plano atualizado")
        _, resp = c.ate("resposta")
        txt = resp["dados"]["resposta_texto"] if resp else ""
        checar(txt.startswith("RESULTADO INCOMPLETO") and "Dados ausentes: resultado:mock.saida_grande" in txt and "Motivo:" in txt and "Próximo passo:" in txt,
               "T4: resposta final diz que NÃO concluiu, o motivo, o dado ausente e o próximo passo", txt[:400])
        checar(time.time() - t0 < 15, "T4: termina em tempo limitado (sem loop de planejamento)")
        c.fechar()

        print("== T5. permitir_falha: passo opcional que falha não impede a conclusão ==")
        c = Conexao(hm)
        c.enviar("solicitar", plano([P("mock.leitura"), P("mock.saida_grande", permitir_falha=True)]))
        msgs, res = c.ate("resultado")
        tar = res["dados"]["dados"]["tarefa"]
        checar(res["dados"]["estado"] == "sucesso" and tar["replanejamentos"] == 0 and [p["estado"] for p in tar["passos"]] == ["sucesso", "falha"],
               "T5: sucesso, sem replanejar, com a falha do passo opcional registrada")
        c.ate("resposta")
        c.fechar()

        print("== T6. timeout por passo ==")
        c = Conexao(hm)
        t0 = time.time()
        c.enviar("solicitar", plano([P("mock.timeout")]))
        msgs, res = c.ate("resultado", 20)
        tar = res["dados"]["dados"]["tarefa"]
        checar(res["dados"]["estado"] == "falha" and all(p["estado"] == "falha" and "limite_excedido" in (p["erro"] or "") for p in tar["passos"]),
               "T6: passo que não termina é interrompido pelo timeout do passo; sem dado nenhum -> 'falha'", [p["erro"] for p in tar["passos"]])
        checar(time.time() - t0 < 8 and tar["replanejamentos"] <= 3, "T6: nova tentativa limitada, sem ficar preso (%.1f s)" % (time.time() - t0))
        _, resp = c.ate("resposta")
        checar(resp and resp["dados"]["resposta_texto"].startswith("RESULTADO SEM DADOS") and "não há conclusão possível" in resp["dados"]["resposta_texto"],
               "T6: sem evidência nenhuma, a resposta final não conclui nada")
        c.fechar()

        print("== T7. cancelamento GLOBAL ==")
        c = Conexao(hm)
        c.enviar("solicitar", plano([P("mock.timeout", timeout_ms=20000), P("mock.timeout", timeout_ms=20000),
                                      P("mock.leitura", depende_de=["passo-1"])]))
        msgs, pl = c.ate("plano")
        exec_id = pl["dados"]["execucao_id"]
        time.sleep(0.3)
        c.enviar("cancelar", {"execucao_id": "exec-de-outra-coisa"})
        msgs, err = c.ate("erro")
        checar(err and err["dados"]["codigo"] == "estado_invalido", "T7: cancelar com execucao_id errado é recusado (não cancela a tarefa errada)")
        t0 = time.time()
        c.enviar("cancelar", {"execucao_id": exec_id})
        msgs, res = c.ate("resultado", 5)
        estados = [p["estado"] for p in res["dados"]["dados"]["tarefa"]["passos"]] if res else []
        checar(res and res["dados"]["estado"] == "cancelado" and estados == ["cancelado", "cancelado", "pulado"] and time.time() - t0 < 1.5,
               "T7: um 'cancelar' para os dois passos em execução e pula o pendente, de imediato", estados)
        quieto, extra = c.silencio(1.0)
        checar(quieto, "T7: nada mais chega depois do cancelamento (nenhum passo continua, nenhuma resposta fantasma)", extra)
        c.enviar("solicitar", plano([P("mock.leitura")]))
        _, res = c.ate("resultado")
        checar(res and res["dados"]["estado"] == "sucesso", "T7: a conexão aceita uma nova tarefa depois do cancelamento")
        c.ate("resposta")
        c.fechar()

        print("== T8–T11. validação integral: nada executa se QUALQUER passo for inválido ==")
        c = Conexao(hm)

        def recusa(dados, codigo, desc, trecho=None):
            c.enviar("solicitar", dados)
            msgs, err = c.ate("erro", 5)
            ok = err and err["dados"]["codigo"] == codigo and (trecho is None or trecho in err["dados"]["mensagem"])
            ok = ok and not any(m["tipo"] in ("plano", "progresso") for m in msgs)
            checar(ok, desc, err["dados"] if err else msgs)
        recusa(plano([P("mock.leitura"), P("ferramenta.que.nao.existe")]), "ferramenta_desconhecida", "T8: ferramenta desconhecida em um dos passos -> plano recusado, nenhum passo roda")
        recusa(plano([P("mock.leitura", id="a", depende_de=["b"]), P("mock.leitura", id="b", depende_de=["a"])]), "plano_invalido",
               "T10: ciclo de dependências -> recusado", "dependência circular")
        recusa(plano([P("mock.leitura", id="a", depende_de=["zzz"])]), "plano_invalido", "T10: dependência de passo inexistente -> recusado")
        recusa(plano([P("mock.leitura") for _ in range(9)]), "plano_invalido", "T11: plano acima de 8 passos -> recusado", "limite de 8")
        recusa(plano([]), "plano_invalido", "T11: plano sem passos -> recusado")
        recusa(plano([P("mock.leitura", parametros=[1, 2])]), "parametro_invalido", "T11: parâmetros que não são objeto -> recusado")
        recusa(plano([P("mock.leitura", id="x"), P("mock.leitura", id="x")]), "plano_invalido", "T11: id de passo repetido -> recusado")
        quieto, extra = c.silencio(0.7)
        checar(quieto, "T8–T11: depois das recusas, nenhuma execução aconteceu", extra)
        c.fechar()

        print("== T9/T12. passo administrativo/escrita: aprovação obrigatória, vinculada ao HASH ==")
        c = Conexao(hm)
        c.enviar("solicitar", plano([P("mock.leitura"), P("mock.admin")]))
        msgs, pl = c.ate("plano")
        d = pl["dados"]
        checar(d["requer_aprovacao"] is True and d["risco"] == "admin" and d["somente_leitura"] is False,
               "T9: um passo administrativo torna o PLANO de risco admin e exige aprovação")
        quieto, extra = c.silencio(0.8)
        checar(quieto, "T9: nenhum passo (nem o de leitura) roda antes da aprovação", extra)
        c.enviar("aprovar", {"plano_id": d["plano_id"], "decisao": "aprovar"})
        _, err = c.ate("erro", 5)
        checar(err and err["dados"]["codigo"] == "estado_invalido" and "hash" in err["dados"]["mensagem"], "T12: aprovar SEM hash é recusado")
        c.enviar("aprovar", {"plano_id": d["plano_id"], "hash": "0" * 64, "decisao": "aprovar"})
        _, err = c.ate("erro", 5)
        checar(err and err["dados"]["codigo"] == "estado_invalido", "T12: aprovar com o plano_id certo e hash ERRADO é recusado")
        quieto, extra = c.silencio(0.6)
        checar(quieto, "T12: depois das aprovações inválidas, nada executou", extra)
        c.enviar("aprovar", {"plano_id": d["plano_id"], "hash": d["hash"], "decisao": "rejeitar"})
        _, res = c.ate("resultado", 5)
        checar(res and res["dados"]["estado"] == "cancelado", "T9: rejeitar encerra a tarefa sem executar nada")
        c.enviar("solicitar", plano([P("mock.leitura"), P("mock.escrita")]))
        _, pl = c.ate("plano")
        d = pl["dados"]
        c.enviar("aprovar", {"plano_id": d["plano_id"], "hash": d["hash"], "decisao": "aprovar"})
        msgs, res = c.ate("resultado", 10)
        checar(res and res["dados"]["estado"] == "sucesso" and momento(msgs, 1, "sucesso") <= momento(msgs, 2, "executando"),
               "T12: com o hash certo executa; o passo de ESCRITA só começa depois que a leitura terminou (nunca em paralelo)")
        c.ate("resposta")
        # fluxo antigo de um passo: mesma regra
        c.enviar("solicitar", {"ferramenta": "mock.escrita", "parametros": {}})
        _, pl = c.ate("plano")
        d = pl["dados"]
        c.enviar("aprovar", {"plano_id": d["plano_id"], "hash": "f" * 64, "decisao": "aprovar"})
        _, err = c.ate("erro", 5)
        checar(err and "hash" in err["dados"]["mensagem"], "T12: plano de um passo (fluxo antigo) também exige o hash certo na aprovação")
        c.enviar("aprovar", {"plano_id": d["plano_id"], "hash": d["hash"], "decisao": "aprovar"})
        _, res = c.ate("resultado", 10)
        checar(res and res["dados"]["estado"] == "sucesso", "T12: com o hash do plano apresentado, a aprovação vale")
        c.fechar()

        print("== T13. plano proposto pelo MODELO: validado igual; errado é rejeitado; aprovado não pode ser trocado ==")
        c = Conexao(hm)
        c.enviar("perguntar", {"mensagem": 'MOCK_PLANO:{"objetivo":"x","passos":[{"ferramenta":"mock.leitura"},{"ferramenta":"sudo.rm_rf"}]}'})
        _, pr = c.ate("proposta")
        d = pr["dados"]
        checar(d["plano_proposto"] is None and d["validacao"]["veredito"] == "rejeitada" and "ferramenta_desconhecida" in d["validacao"]["motivo"],
               "T13: modelo propondo ferramenta errada num plano -> plano inteiro rejeitado, nada executável", d["validacao"])
        c.enviar("perguntar", {"mensagem": 'MOCK_PLANO:{"objetivo":"x","passos":[{"id":"a","ferramenta":"mock.leitura","depende_de":["b"]},{"id":"b","ferramenta":"mock.leitura","depende_de":["a"]}]}'})
        _, pr = c.ate("proposta")
        checar(pr["dados"]["plano_proposto"] is None and "circular" in pr["dados"]["validacao"]["motivo"], "T13: plano do modelo com ciclo -> rejeitado")
        c.enviar("perguntar", {"mensagem": 'MOCK_PLANO:{"objetivo":"elevar","passos":[{"ferramenta":"mock.leitura"},{"ferramenta":"mock.admin"}]}'})
        _, pr = c.ate("proposta")
        d = pr["dados"]
        pa = d["plano_proposto"]
        checar(pa and d["validacao"]["execucao_automatica_permitida"] is False and d["validacao"]["requer_aprovacao"] is True and pa["risco"] == "admin",
               "T13: modelo tentando criar passo ADMINISTRATIVO -> o plano nunca é automático e exige aprovação humana", d["validacao"])
        quieto, extra = c.silencio(0.6)
        checar(quieto, "T13: a proposta do modelo, sozinha, não executa nada", extra)
        # o modelo "muda de ideia": novo plano B substitui o proposto; o hash de A não vale para B, e A não existe mais
        c.enviar("perguntar", {"mensagem": 'MOCK_PLANO:{"objetivo":"outro","passos":[{"ferramenta":"mock.leitura"}]}'})
        _, pr = c.ate("proposta")
        pb = pr["dados"]["plano_proposto"]
        c.enviar("solicitar", {"plano_id": pb["plano_id"], "hash": pa["hash"]})
        _, err = c.ate("erro", 5)
        checar(err and err["dados"]["codigo"] == "plano_alterado", "T13: executar o plano B com o hash do plano A -> 'plano_alterado'")
        c.enviar("solicitar", {"plano_id": pa["plano_id"], "hash": pa["hash"]})
        _, err = c.ate("erro", 5)
        checar(err and err["dados"]["codigo"] == "estado_invalido", "T13: o plano A, já substituído, não pode mais ser executado")
        c.enviar("solicitar", {"plano_id": pb["plano_id"], "hash": pb["hash"]})
        msgs, pl = c.ate("plano")
        checar(pl and pl["dados"]["hash"] == pb["hash"] and [p["ferramenta"] for p in pl["dados"]["passos"]] == ["mock.leitura"],
               "T13: o que executa é exatamente o plano validado e guardado no daemon (mesmo hash, mesmos passos)")
        _, res = c.ate("resultado")
        checar(res and res["dados"]["estado"] == "sucesso", "T13: plano do modelo, validado e só de leitura, executa")
        c.ate("resposta")
        c.enviar("solicitar", {"plano_id": pb["plano_id"], "hash": pb["hash"]})
        _, err = c.ate("erro", 5)
        checar(err is not None, "T13: o mesmo plano não pode ser executado duas vezes (sem execução duplicada)")
        c.fechar()

        print("== T14. replanejamento: sugestão ADMINISTRATIVA do modelo é recusada pelo validador ==")
        c = Conexao(hm)
        c.enviar("solicitar", plano([P("mock.saida_grande")], objetivo="MOCK_PROPOR:mock.admin"))
        msgs, res = c.ate("resultado", 20)
        tar = res["dados"]["dados"]["tarefa"]
        checar(not any(p["ferramenta"] == "mock.admin" for p in tar["passos"]) and
               not any(m["tipo"] == "progresso" and m["dados"]["tarefa"].get("passo", {}).get("ferramenta") == "mock.admin" for m in msgs),
               "T14: o passo administrativo sugerido pelo modelo no replanejamento NUNCA entra no plano nem executa")
        checar(any("sugestão do modelo recusada" in l and "não é leitura pura" in l for l in tar["limitacoes"]),
               "T14: a recusa fica registrada como limitação da tarefa", tar["limitacoes"])
        _, resp = c.ate("resposta")
        checar(resp and "sugestão do modelo recusada" in resp["dados"]["resposta_texto"], "T14: e aparece na resposta final")
        c.fechar()

        print("== T15. orçamento de passos esgotado ==")
        c = Conexao(hm)
        c.enviar("solicitar", plano([P("mock.leitura") for _ in range(7)] + [P("mock.saida_grande")]))
        msgs, res = c.ate("resultado", 20)
        tar = res["dados"]["dados"]["tarefa"]
        checar(res["dados"]["estado"] == "incompleto" and len(tar["passos"]) == 8 and tar["replanejamentos"] == 0,
               "T15: com os 8 passos usados não há replanejamento — termina 'incompleto'")
        _, resp = c.ate("resposta")
        checar("8 passo(s) usados de 8" in resp["dados"]["resposta_texto"], "T15: a resposta final cita o orçamento esgotado")
        c.fechar()

        print("== T16. desconectar no meio da tarefa ==")
        c = Conexao(hm)
        c.enviar("solicitar", plano([P("mock.timeout", timeout_ms=20000), P("mock.leitura")]))
        c.ate("plano")
        time.sleep(0.2)
        c.fechar()
        time.sleep(0.6)
        log = open(os.path.join(hm, "sessiond.log")).read()
        checar(dm.poll() is None and "cancelada (cliente desconectou)" in log, "T16: o daemon cancela a tarefa e continua vivo")
        c = Conexao(hm)
        c.enviar("solicitar", plano([P("mock.leitura")]))
        _, res = c.ate("resultado")
        checar(res and res["dados"]["estado"] == "sucesso", "T16: uma nova conexão executa normalmente em seguida")
        c.fechar()
        parar(dm)
        dm = None

        # ------------------------------------------------ executor real, modelo mock
        # LIA_SESSIOND_LEITURA_DO_MODELO=sim: a leitura opcional do modelo fica ligada
        # neste daemon para exercitar a conferência dela (T20); o padrão (desligada) é o T20b.
        dr = iniciar_daemon(hr, LIA_SESSIOND_EXECUTOR="real", LIA_SESSIOND_LEITURA_DO_MODELO="sim")

        print("== T17. tarefa real: 'por que o sistema está lento' -> 4 passos, dados reais, relatório ==")
        c = Conexao(hr)
        c.enviar("perguntar", {"mensagem": "Analise por que o sistema pode estar lento agora."})
        _, pr = c.ate("proposta")
        d = pr["dados"]
        pl = d["plano_proposto"]
        fer = [p["ferramenta"] for p in pl["passos"]] if pl else []
        checar(d["tipo_resposta"] == "tarefa" and fer == ["sistema.recursos", "sistema.processos_top_cpu", "sistema.processos_top_memoria", "sistema.dispositivos"],
               "T17: lentidão não escolhe só sistema.recursos — plano com recursos, CPU, memória e dispositivos", fer)
        checar(d["validacao"]["execucao_automatica_permitida"] is True and d["validacao"]["compatibilidade"] == "compativel" and d["ferramenta_proposta"] is None,
               "T17: plano somente leitura -> execução automática permitida")
        quieto, extra = c.silencio(0.5)
        checar(quieto, "T17: a proposta em si não executa nada", extra)
        c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": d["pergunta_id"]})
        msgs, res = c.ate("resultado", 30)
        tar = res["dados"]["dados"]["tarefa"]
        checar(res["dados"]["estado"] == "sucesso" and all(p["estado"] == "sucesso" for p in tar["passos"]), "T17: os 4 passos reais concluem; verificador: sucesso")
        ev = set(e for p in tar["passos"] for e in p["evidencias"])
        checar({"carga_media", "memoria_disponivel", "swap", "lista_processos_cpu", "lista_processos_memoria"} <= ev, "T17: evidências exigidas presentes", sorted(ev))
        _, resp = c.ate("resposta", 30)
        txt = resp["dados"]["resposta_texto"]
        mem_total = int(next(l for l in open("/proc/meminfo") if l.startswith("MemTotal:")).split()[1]) // 1024
        top = tar["passos"][2]["dados"]["saida"]["processos"][0]
        checar("de %d MiB" % mem_total in txt and "%s (PID %d)" % (top["nome"], top["pid"]) in txt,
               "T17: o relatório cita a RAM total de /proc/meminfo (%d MiB) e o processo nº 1 do JSON real" % mem_total)
        checar(resp["dados"]["pergunta_id"] == d["pergunta_id"] and all(s in txt for s in ("Conclusão:", "Evidências", "Limitações:", "Ações realizadas", "Ações não realizadas", "Próximo passo:")),
               "T17: resposta final correlacionada à pergunta, com as seis seções")
        c.fechar()

        print("== T18. evidência ausente -> replanejamento determinístico busca em outra ferramenta ==")
        c = Conexao(hr)
        c.enviar("solicitar", plano([P("sistema.diagnosticar")], criterios_sucesso=["kernel", "carga_media"]))
        msgs, res = c.ate("resultado", 30)
        tar = res["dados"]["dados"]["tarefa"]
        checar(res["dados"]["estado"] == "sucesso" and tar["replanejamentos"] == 1 and [p["ferramenta"] for p in tar["passos"]] == ["sistema.diagnosticar", "sistema.recursos"] and
               tar["passos"][1]["de_replanejamento"], "T18: faltava 'carga_media' -> um ciclo acrescenta sistema.recursos -> sucesso", [p["ferramenta"] for p in tar["passos"]])
        checar(tar["versao"] == 2, "T18: o plano replanejado tem nova versão (e novo hash)")
        c.ate("resposta")
        c.enviar("solicitar", plano([P("sistema.processos_top_cpu", parametros={"limite": 3})], criterios_sucesso=["lista_processos_cpu", "evidencia_que_ninguem_fornece"]))
        msgs, res = c.ate("resultado", 40)
        checar(res["dados"]["estado"] == "incompleto" and res["dados"]["dados"]["evidencias_ausentes"] == ["evidencia_que_ninguem_fornece"],
               "T18: evidência que nenhuma ferramenta fornece -> 'incompleto' com o dado ausente nomeado (nunca sucesso)")
        c.ate("resposta")
        c.fechar()

        print("== T19. malware: nenhuma ferramenta 'parecida' no lugar; coleta limitada só quando pedida ==")
        c = Conexao(hr)
        for pergunta in ("Existe vírus nesta máquina?", "me diga se tem virus instalado nessa maquina"):
            c.enviar("perguntar", {"mensagem": pergunta})
            _, pr = c.ate("proposta")
            d = pr["dados"]
            checar(d["plano_proposto"] is None and d["ferramenta_proposta"] is None and d["validacao"]["compatibilidade"] == "inexistente" and
                   "Não há ferramenta de detecção de malware" in d["resposta_texto"] and "tools.discover" not in json.dumps(d),
                   "T19: \"%s\" -> resposta direta, ferramenta INEXISTENTE, sem tools.discover" % pergunta, d["validacao"])
        quieto, extra = c.silencio(0.5)
        checar(quieto, "T19: nada foi executado para a pergunta de vírus", extra)
        c.enviar("perguntar", {"mensagem": "Explique quais dados são insuficientes para afirmar que há malware."})
        _, pr = c.ate("proposta")
        checar(pr["dados"]["plano_proposto"] is None and "insuficientes" in pr["dados"]["resposta_texto"] and "nunca uma garantia" in pr["dados"]["resposta_texto"],
               "T19: 'explique o que falta' -> explicação, sem executar")
        c.enviar("perguntar", {"mensagem": "Liste processos suspeitos, mas não diga que são vírus sem evidência."})
        _, pr = c.ate("proposta")
        d = pr["dados"]
        pl = d["plano_proposto"]
        checar(pl and d["validacao"]["compatibilidade"] == "parcial" and len(pl["limitacoes"]) >= 3, "T19: coleta pedida -> tarefa de compatibilidade PARCIAL, com limitações declaradas")
        c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": d["pergunta_id"]})
        c.ate("resultado", 30)
        _, resp = c.ate("resposta", 30)
        txt = resp["dados"]["resposta_texto"]
        checar("Não é possível afirmar se há ou não malware" in txt and "nenhum deles é classificado como vírus" in txt and "não existe ferramenta de detecção de malware" in txt,
               "T19: o relatório não declara infectado nem seguro, e diz o que não foi verificado")
        c.fechar()

        print("== T20. leitura do modelo: aceita só se conferida; invenção e afirmação de segurança são descartadas ==")
        for marcador, trecho, desc in (("MOCK_RESUMO_INVENTADO", "citava o número", "número que não está nas evidências"),
                                       ("MOCK_RESUMO_SEGURO", "afirmação absoluta de segurança", "\"está livre de vírus\""),
                                       ("MOCK_RESUMO_POSITIVO", "concluía que está tudo bem", "\"está sem problemas\""),
                                       ("MOCK_RESUMO_RECUSA", "negava ter os dados", "negação dos dados")):
            c = Conexao(hr)
            c.enviar("perguntar", {"mensagem": "Compare os 5 processos que mais usam CPU e memória. " + marcador})
            _, pr = c.ate("proposta")
            d = pr["dados"]
            pl = d["plano_proposto"]
            c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": d["pergunta_id"]})
            c.ate("resultado", 30)
            _, resp = c.ate("resposta", 30)
            r = resp["dados"]
            checar(r["origem"] == "deterministica" and trecho in (r["aviso"] or "") and "(mock)" not in r["resposta_texto"] and "Conclusão:" in r["resposta_texto"],
                   "T20: leitura do modelo com %s -> descartada; o relatório determinístico segue inteiro" % desc, r["aviso"])
            c.fechar()
        c = Conexao(hr)
        c.enviar("perguntar", {"mensagem": "Compare os 5 processos que mais usam CPU e memória."})
        _, pr = c.ate("proposta")
        d = pr["dados"]
        pl = d["plano_proposto"]
        checar(all(p["parametros"] == {"limite": 5} for p in pl["passos"]), "T20: o limite dito pela pessoa (5) vale para os dois passos")
        c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": d["pergunta_id"]})
        c.ate("resultado", 30)
        _, resp = c.ate("resposta", 30)
        checar(resp["dados"]["origem"] == "deterministica+modelo" and "Leitura do modelo (texto livre; números conferidos" in resp["dados"]["resposta_texto"],
               "T20: leitura do modelo sem problema é aceita, em seção própria e rotulada")
        c.fechar()

        print("== T20b. padrão: leitura do modelo DESLIGADA — a resposta é o relatório determinístico, na hora ==")
        parar(dr)
        dr = iniciar_daemon(hr, LIA_SESSIOND_EXECUTOR="real", LIA_SESSIOND_MODELO="ollama", LIA_OLLAMA_PORTA="1")  # modelo fora do ar
        c = Conexao(hr)
        t0 = time.time()
        c.enviar("perguntar", {"mensagem": "Analise por que o sistema pode estar lento agora."})
        _, pr = c.ate("proposta")
        d = pr["dados"]
        pl = d["plano_proposto"]
        c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": d["pergunta_id"]})
        c.ate("resultado", 30)
        _, resp = c.ate("resposta", 30)
        dur = time.time() - t0
        checar(resp and resp["dados"]["origem"] == "deterministica" and resp["dados"]["aviso"] is None and "Leitura do modelo" not in resp["dados"]["resposta_texto"] and
               "Conclusão:" in resp["dados"]["resposta_texto"],
               "T20b: sem a leitura opcional, a tarefa inteira (plano, 4 passos, verificação, relatório) não depende do modelo")
        checar(dur < 5, "T20b: tarefa de 4 passos reais concluída em %.2f s, com o modelo fora do ar" % dur)
        c.fechar()

        print("== T21. consultas simples continuam no fluxo de um passo ==")
        c = Conexao(hr)
        c.enviar("perguntar", {"mensagem": "Liste os 10 processos que mais consomem memória RAM agora."})
        _, pr = c.ate("proposta")
        checar(pr["dados"].get("plano_proposto") is None and pr["dados"]["ferramenta_proposta"]["ferramenta"] == "sistema.processos_top_memoria",
               "T21: pergunta simples não vira tarefa de vários passos")
        c.fechar()
    finally:
        parar(dm)
        parar(dr)

    print("== T22. isolamento ==")
    alterado = False
    lia_real = os.path.join(home_real, ".lia")
    if os.path.isdir(lia_real):
        for raiz_d, _, arqs in os.walk(lia_real):
            for a in arqs:
                try:
                    if os.path.getmtime(os.path.join(raiz_d, a)) > marca:
                        alterado = True
                except OSError:
                    pass
    checar(not alterado, "T22: nada em ~/.lia real foi criado/alterado")
    restos = [p for p in subprocess.run(["pgrep", "-f", base], capture_output=True, text=True).stdout.split() if p != str(os.getpid())]
    checar(not restos, "T22: nenhum processo residual", restos)
    checar(not any(os.path.exists(os.path.join(h, ".lia", "run", "session.sock")) for h in (hm, hr)), "T22: sockets removidos")
    if not os.environ.get("LIA_DEBUG_MANTER"):
        subprocess.run(["rm", "-rf", base])
    print("\n%d testes, %d falhas" % (total, falhas))
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
