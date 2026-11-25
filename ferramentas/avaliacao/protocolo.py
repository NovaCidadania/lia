"""Protocolo do avaliador com o lia-sessiond (lia.v1, NDJSON em socket unix).

Sequência real de uma tarefa com plano (docs/protocolo.md, sessiond.c):

  perguntar -> proposta(plano_proposto{plano_id,hash}) -> solicitar(plano_id, hash,
  origem_modelo, pergunta_id) -> plano(plano_id,hash) -> aprovar|rejeitar(hash)
  -> progresso... -> resultado -> resposta

e a do ciclo de coding: perguntar -> proposta(codigo) -> progresso... -> plano(s)
-> aprovar -> resposta.  Uma 'proposta' SEM plano e SEM codigo é a resposta final
em texto.

Todo desfecho que NÃO é uma resposta do daemon (silêncio, desconexão, 'erro',
evento desconhecido) é um desfecho de HARNESS com diagnóstico — nunca uma falha do
modelo e nunca uma espera de 40 minutos (auditoria 2026-11-25)."""
import os, signal, time

# o que o daemon pode enviar ao cliente (gui/protocolo.h); qualquer outro tipo é inesperado
TIPOS_DO_DAEMON = {"plano", "progresso", "saida", "resultado", "auditoria", "erro", "proposta", "resposta"}
TERMINOS_DE_HARNESS = ("sem_progresso", "desconectado", "erro_protocolo", "evento_inesperado", "handshake_repetido")


def conduzir(c, prompt, decidir_plano, limite_s, sem_progresso_s, agora=time.time):
    """Envia 'perguntar' e conduz o protocolo até um desfecho.

    decidir_plano(dados_do_plano) -> (decisao, motivo, passos) — a política de aprovação do chamador.
    Retorna dict: termino ('resposta' | 'limite_tempo' | um de TERMINOS_DE_HARNESS), resposta,
    eventos, aprovacoes, solicitacoes, diagnostico (texto, vazio se 'resposta')."""
    out = {"termino": None, "resposta": None, "eventos": [], "aprovacoes": [], "solicitacoes": [], "diagnostico": ""}
    t0 = agora()
    ultimo = t0
    solicitados = set()
    c.enviar("perguntar", {"mensagem": prompt})
    while out["termino"] is None:
        if agora() - t0 >= limite_s:
            out["termino"] = "limite_tempo"
            out["diagnostico"] = "limite de %d s sem resposta final (o daemon continuava emitindo eventos)" % limite_s
            break
        if agora() - ultimo >= sem_progresso_s:
            ult = out["eventos"][-1]["tipo"] if out["eventos"] else "(nenhum)"
            out["termino"] = "sem_progresso"
            out["diagnostico"] = "nenhum evento do daemon em %d s; último evento: %s" % (sem_progresso_s, ult)
            break
        msgs, _ = c.ate("__nada__", min(2.0, max(0.1, sem_progresso_s)))
        if getattr(c, "fechado", False) and not msgs:
            out["termino"] = "desconectado"
            out["diagnostico"] = "o daemon fechou a conexão antes da resposta final"
            break
        for m in msgs:
            ultimo = agora()
            tipo = m.get("tipo")
            dd = m.get("dados") or {}
            ev = {"t": round(agora() - t0, 1), "tipo": tipo}
            out["eventos"].append(ev)
            if tipo not in TIPOS_DO_DAEMON:
                out["termino"], out["diagnostico"] = "evento_inesperado", "tipo de evento desconhecido: %r" % (tipo,)
                break
            if tipo == "progresso":
                ev["mensagem"] = dd.get("mensagem")
                if "codigo" in dd:
                    ev["etapa"] = (dd["codigo"] or {}).get("etapa")
            elif tipo == "erro":
                ev["codigo"], ev["mensagem"] = dd.get("codigo"), dd.get("mensagem")
                out["termino"] = "erro_protocolo"
                out["diagnostico"] = "o daemon respondeu 'erro': %s — %s" % (dd.get("codigo"), (dd.get("mensagem") or "")[:200])
                break
            elif tipo == "plano":
                ev["intencao"] = dd.get("intencao")
                decisao, motivo, passos = decidir_plano(dd)
                out["aprovacoes"].append({"t": ev["t"], "intencao": dd.get("intencao"), "decisao": decisao, "motivo": motivo, "passos": passos,
                                          "plano_id": dd.get("plano_id"), "hash": dd.get("hash")})
                c.enviar("aprovar", {"plano_id": dd["plano_id"], "hash": dd["hash"], "decisao": decisao, "escopo": "uma_vez"})
            elif tipo == "resposta":
                out["termino"], out["resposta"] = "resposta", dd.get("resposta_texto")
                break
            elif tipo == "proposta":
                pl = dd.get("plano_proposto")
                if pl:
                    # o plano validado só executa com 'solicitar' presos ao id e ao hash que o daemon enviou
                    ev["plano_proposto"] = {"plano_id": pl.get("plano_id"), "hash": pl.get("hash"), "intencao": pl.get("intencao")}
                    if pl.get("plano_id") in solicitados:
                        out["termino"], out["diagnostico"] = "handshake_repetido", "o daemon propôs de novo o plano %s já solicitado" % pl.get("plano_id")
                        break
                    solicitados.add(pl.get("plano_id"))
                    sol = {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": dd.get("pergunta_id")}
                    out["solicitacoes"].append({"t": ev["t"], **sol})
                    c.enviar("solicitar", sol)
                elif not dd.get("codigo"):
                    out["termino"] = "resposta"
                    out["resposta"] = dd.get("resposta_texto") or "(proposta sem tarefa de código)"
                    break
    return out


def descendentes(pid):
    """pids de todos os descendentes de 'pid' (inclusive os que abriram sessão própria), lendo /proc"""
    filhos = {}
    for n in os.listdir("/proc"):
        if not n.isdigit():
            continue
        try:
            st = open("/proc/%s/stat" % n).read()
            ppid = int(st[st.rindex(")") + 2:].split()[1])
        except (OSError, ValueError):
            continue
        filhos.setdefault(ppid, []).append(int(n))
    achados, pilha = [], [pid]
    while pilha:
        for f in filhos.get(pilha.pop(), []):
            achados.append(f)
            pilha.append(f)
    return achados


def _vivo(pid):
    try:
        os.kill(pid, 0)
        with open("/proc/%d/stat" % pid) as f:
            s = f.read()
        return s[s.rindex(")") + 2] != "Z"
    except (OSError, ValueError):
        return False


def encerrar_arvore(p, espera=5.0):
    """Encerra o processo 'p' (subprocess.Popen, iniciado com start_new_session) e TODA a descendência:
    SIGTERM no grupo e em cada descendente, depois SIGKILL nos que restarem. Idempotente."""
    pid = p.pid
    alvos = descendentes(pid) if p.poll() is None or _vivo(pid) else []
    alvos = [pid] + alvos
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(pid, sig)
        except (ProcessLookupError, PermissionError):
            pass
        for a in alvos:
            try:
                os.kill(a, sig)
            except (ProcessLookupError, PermissionError):
                pass
        fim = time.time() + (espera if sig == signal.SIGTERM else 2.0)
        while time.time() < fim and p.poll() is None:
            time.sleep(0.05)
        try:
            p.wait(timeout=0.1)
        except Exception:
            pass
        restantes = [a for a in alvos if a != pid and _vivo(a)]
        fim = time.time() + (espera if sig == signal.SIGTERM else 2.0)
        while restantes and time.time() < fim:
            time.sleep(0.05)
            restantes = [a for a in restantes if _vivo(a)]
        if p.poll() is not None and not restantes:
            return True
    return p.poll() is not None and not any(_vivo(a) for a in alvos if a != pid)
