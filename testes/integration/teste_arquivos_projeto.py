#!/usr/bin/env python3
"""Arquivos do projeto de ponta a ponta: daemon real (executor real,
modelo mock), projeto temporário sob /tmp (LIA_SESSIOND_PROJETO).

Confere: leitura/listagem/busca sem aprovação; TODA escrita com plano que
traz a prévia (diff), nada gravado antes de 'aprovar', rejeição não altera
nada, arquivo alterado entre a prévia e a aprovação não é sobrescrito,
fora do projeto e .git/ recusados, desfazer, auditoria, sem projeto
definido, codificação legada, e o lia-chat mostrando a prévia e pedindo
confirmação. Uso: make teste-arquivos-projeto"""
import json, os, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from teste_web_resposta import Conexao, parar  # noqa: E402

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SESSIOND = os.path.join(RAIZ, "build", "lia-sessiond")
CHAT = os.path.join(RAIZ, "build", "lia-chat")
total = falhas = 0


def checar(cond, desc, detalhe=""):
    global total, falhas
    total += 1
    if cond:
        print("ok   " + desc, flush=True)
    else:
        falhas += 1
        print("FALHA " + desc + (("\n      " + str(detalhe)[:1500]) if detalhe != "" else ""), flush=True)


def iniciar(home, projeto):
    env = {"HOME": home, "PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LIA_SESSIOND_EXECUTOR": "real"}
    if projeto:
        env["LIA_SESSIOND_PROJETO"] = projeto
    for k in ("LIA_SESSIOND_SEM_RLIMIT_PARA_TESTE", "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"):
        if os.environ.get(k):
            env[k] = os.environ[k]
    p = subprocess.Popen([SESSIOND], env=env, stdout=open(os.path.join(home, "sessiond.log"), "a"), stderr=subprocess.STDOUT,
                         start_new_session=True)
    sock = os.path.join(home, ".lia", "run", "session.sock")
    for _ in range(200):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    return p


def perguntar(c, texto):
    c.enviar("perguntar", {"mensagem": texto})
    _, pr = c.ate("proposta")
    return pr["dados"] if pr else {}


def solicitar_plano(c, pr):
    pl = pr["plano_proposto"]
    c.enviar("solicitar", {"plano_id": pl["plano_id"], "hash": pl["hash"], "origem_modelo": True, "pergunta_id": pr["pergunta_id"]})
    _, plano = c.ate("plano")
    return plano["dados"] if plano else {}


def decidir(c, plano, decisao):
    c.enviar("aprovar", {"plano_id": plano["plano_id"], "hash": plano["hash"], "decisao": decisao, "escopo": "uma_vez"})
    msgs1, res = c.ate("resultado", 15)
    msgs2, resp = c.ate("resposta", 10)
    return (res["dados"] if res else {}), (resp["dados"] if resp else {}), msgs1 + msgs2


def ler(caminho):
    with open(caminho, "rb") as f:
        return f.read()


def apresentacao_do_passo(msgs):
    for m in msgs:
        t = m["dados"].get("tarefa") if m["tipo"] == "progresso" else None
        if t and t.get("passo") and t["passo"].get("apresentacao"):
            return t["passo"]["apresentacao"]
    return ""


def main():
    if not os.access(SESSIOND, os.X_OK):
        print("lia-sessiond não encontrado — rode 'make teste-arquivos-projeto'")
        return 1
    base = tempfile.mkdtemp(prefix="lia-arqp-", dir="/tmp")
    proj = os.path.join(base, "projeto")
    os.makedirs(os.path.join(proj, "src"))
    os.makedirs(os.path.join(proj, ".git"))
    with open(os.path.join(proj, ".git", "config"), "w") as f:
        f.write("[core]\n")
    with open(os.path.join(proj, "src", "main.c"), "w") as f:
        f.write("#include <stdio.h>\n\nint main(void)\n{\n    printf(\"olá\\n\");\n    return 0;\n}\n")
    with open(os.path.join(proj, "src", "util.c"), "w") as f:
        f.write("int soma(int a, int b) { return a + b; } /* TODO: overflow */\n")
    with open(os.path.join(proj, "legado.txt"), "wb") as f:
        f.write("Função de média: aprovação e reprovação.\n".encode("cp1252"))
    with open(os.path.join(base, "fora.txt"), "w") as f:
        f.write("segredo\n")
    home = os.path.join(base, "home")
    os.makedirs(home)
    daemons = []
    print("ambiente isolado:", base)
    try:
        d = iniciar(home, proj)
        daemons.append(d)
        c = Conexao(home)

        print("== F1. leitura sem aprovação, com linhas numeradas ==")
        pr = perguntar(c, "leia o arquivo src/main.c")
        pl = pr.get("plano_proposto") or {}
        checar(pl.get("passos", [{}])[0].get("ferramenta") == "arquivo.ler" and pr["validacao"].get("execucao_automatica_permitida") is True,
               "F1: 'leia o arquivo' vira arquivo.ler, leitura pura que roda sem confirmação", pr.get("validacao"))
        plano = solicitar_plano(c, pr)
        msgs, res = c.ate("resultado", 10)
        _, resp = c.ate("resposta", 10)
        ap = apresentacao_do_passo(msgs)
        checar(plano.get("requer_aprovacao") is False and res and res["dados"]["estado"] == "sucesso", "F1: executa sem 'aprovar'")
        checar("5 │     printf(\"olá\\n\");" in ap and "UTF-8" in ap, "F1: conteúdo com número de linha e codificação", ap[:500])
        checar(resp and resp["dados"]["resposta_texto"].startswith("Mostrei src/main.c: linhas 1 a 7 de 7"), "F1: resposta final curta",
               resp and resp["dados"])

        print("== F2. listar e buscar ==")
        pr = perguntar(c, "liste os arquivos do projeto")
        solicitar_plano(c, pr)
        msgs, _ = c.ate("resultado", 10)
        c.ate("resposta", 10)
        ap = apresentacao_do_passo(msgs)
        checar("src/" in ap and "legado.txt" in ap and ".git" not in ap, "F2: lista o projeto sem .git", ap)
        pr = perguntar(c, "procure \"TODO\" no projeto")
        solicitar_plano(c, pr)
        msgs, _ = c.ate("resultado", 10)
        c.ate("resposta", 10)
        ap = apresentacao_do_passo(msgs)
        checar("src/util.c:1:" in ap and "TODO: overflow" in ap, "F2: busca com arquivo e linha", ap)

        print("== F3. codificação legada lida corretamente ==")
        pr = perguntar(c, "leia legado.txt")
        solicitar_plano(c, pr)
        msgs, _ = c.ate("resultado", 10)
        c.ate("resposta", 10)
        ap = apresentacao_do_passo(msgs)
        checar("Função de média: aprovação e reprovação." in ap and ("WINDOWS-1252" in ap or "ISO-8859-1" in ap), "F3: Windows-1252 decodificado", ap)

        print("== F4. criar: prévia antes, nada gravado até aprovar ==")
        alvo = os.path.join(proj, "src", "ola.c")
        pr = perguntar(c, "crie o arquivo src/ola.c com\n```c\nint ola(void) { return 42; }\n```")
        pl = pr.get("plano_proposto") or {}
        checar(pl.get("passos", [{}])[0].get("ferramenta") == "arquivo.criar" and pr["validacao"].get("requer_aprovacao") is True,
               "F4: criação exige aprovação", pr.get("validacao"))
        plano = solicitar_plano(c, pr)
        previas = plano.get("previas") or []
        checar(plano.get("requer_aprovacao") is True and previas and "+int ola(void) { return 42; }" in previas[0]["previa"],
               "F4: o plano traz a prévia (diff) do que vai ser criado", plano)
        checar(not os.path.exists(alvo), "F4: nada foi gravado antes da aprovação")
        res, resp, msgs = decidir(c, plano, "aprovar")
        checar(res.get("estado") == "sucesso" and ler(alvo) == b"int ola(void) { return 42; }\n", "F4: aprovado -> arquivo criado", res)
        txt = resp.get("resposta_texto", "")
        checar(txt.startswith("Feito: criar src/ola.c") and "desfaça a operação" in txt, "F4: resposta diz o que foi feito e como desfazer", txt)
        op_id = txt.split("desfaça a operação ")[-1].rstrip('".')
        aud = [m for m in msgs if m["tipo"] == "auditoria"]
        checar(aud and aud[0]["dados"].get("aprovacao") == "aprovada por uma pessoa", "F4: auditoria: aprovada por uma pessoa", aud)

        print("== F5. rejeitar não altera nada ==")
        antes = ler(os.path.join(proj, "src", "main.c"))
        pr = perguntar(c, "substitua \"return 0;\" por \"return 1;\" em src/main.c")
        plano = solicitar_plano(c, pr)
        pv = (plano.get("previas") or [{}])[0].get("previa", "")
        checar("-    return 0;" in pv and "+    return 1;" in pv, "F5: prévia da edição com - e +", pv)
        res, resp, _ = decidir(c, plano, "rejeitar")
        checar(ler(os.path.join(proj, "src", "main.c")) == antes and res.get("estado") == "cancelado", "F5: rejeitado -> arquivo intacto", res)

        print("== F6. arquivo mudou entre a prévia e a aprovação: nada é sobrescrito ==")
        pr = perguntar(c, "substitua \"return 0;\" por \"return 2;\" em src/main.c")
        plano = solicitar_plano(c, pr)
        with open(os.path.join(proj, "src", "main.c"), "a") as f:
            f.write("/* editado por outra pessoa */\n")
        mudado = ler(os.path.join(proj, "src", "main.c"))
        res, resp, _ = decidir(c, plano, "aprovar")
        checar(ler(os.path.join(proj, "src", "main.c")) == mudado and b"return 2;" not in mudado, "F6: a edição de fora foi preservada")
        checar("mudaram desde a prévia aprovada" in resp.get("resposta_texto", "") and res.get("estado") != "sucesso",
               "F6: a resposta explica que nada foi alterado", resp)

        print("== F7. alteração impossível: recusada antes de pedir aprovação ==")
        pr = perguntar(c, "substitua \"nao existe\" por \"x\" em src/main.c")
        checar(not pr.get("plano_proposto") and "não foi encontrado" in pr.get("resposta_texto", ""), "F7: trecho ausente -> explicação, sem plano", pr)
        pr = perguntar(c, "crie o arquivo src/main.c com o conteúdo: x")
        checar(not pr.get("plano_proposto") and "já existe" in pr.get("resposta_texto", ""), "F7: criar por cima de arquivo existente -> recusado", pr)

        print("== F8. fora do projeto e .git/ ==")
        pr = perguntar(c, "apague o arquivo ../fora.txt")
        checar(not pr.get("plano_proposto") and "fora do projeto" in pr.get("resposta_texto", ""), "F8: apagar fora do projeto recusado", pr)
        pr = perguntar(c, "leia /etc/passwd")
        if pr.get("plano_proposto"):
            solicitar_plano(c, pr)
            _, res = c.ate("resultado", 10)
            _, resp = c.ate("resposta", 10)
            checar(res and res["dados"]["estado"] != "sucesso" and "fora do projeto" in resp["dados"]["resposta_texto"], "F8: ler /etc/passwd recusado",
                   resp and resp["dados"])
        else:
            checar("fora do projeto" in pr.get("resposta_texto", ""), "F8: ler /etc/passwd recusado", pr)
        pr = perguntar(c, "crie o arquivo .git/hooks/pre-commit com o conteúdo: rm -rf /")
        checar(not pr.get("plano_proposto") and "Git" in pr.get("resposta_texto", ""), "F8: escrever em .git/ recusado", pr)
        checar(os.path.exists(os.path.join(base, "fora.txt")), "F8: arquivo de fora continua lá")

        print("== F9. desfazer pela conversa ==")
        pr = perguntar(c, "desfaça a operação %s" % op_id)
        plano = solicitar_plano(c, pr)
        pv = (plano.get("previas") or [{}])[0].get("previa", "")
        checar("remover o arquivo criado: src/ola.c" in pv, "F9: prévia do desfazer", pv)
        res, resp, _ = decidir(c, plano, "aprovar")
        checar(not os.path.exists(alvo) and res.get("estado") == "sucesso", "F9: arquivo criado foi removido", resp)

        print("== F10. solicitar direto (sem planejador): mesma prévia e aprovação ==")
        c.enviar("solicitar", {"ferramenta": "arquivo.anexar", "parametros": {"caminho": "src/util.c", "conteudo": "int dobro(int x);\n"}})
        _, p = c.ate("plano")
        checar(p and p["dados"]["requer_aprovacao"] is True and "+int dobro(int x);" in p["dados"].get("previa", ""), "F10: plano de ferramenta isolada com prévia",
               p and p["dados"])
        c.enviar("aprovar", {"plano_id": p["dados"]["plano_id"], "hash": p["dados"]["hash"], "decisao": "aprovar", "escopo": "uma_vez"})
        _, r = c.ate("resultado", 10)
        checar(r and r["dados"]["estado"] == "sucesso" and ler(os.path.join(proj, "src", "util.c")).endswith(b"int dobro(int x);\n"), "F10: anexado após aprovar")
        c.enviar("solicitar", {"ferramenta": "arquivo.apagar", "parametros": {"caminho": "../fora.txt"}})
        _, e = c.ate("erro", 5)
        checar(e and "fora do projeto" in e["dados"]["mensagem"], "F10: solicitar direto fora do projeto -> erro antes do plano", e)
        c.fechar()

        logs = "".join(open(os.path.join(home, ".lia", "humano", "auditoria", f)).read() for f in os.listdir(os.path.join(home, ".lia", "humano", "auditoria")))
        checar("acao=sessiond.arquivo.criar" in logs and "acao=sessiond.arquivo.anexar" in logs and "olá" not in logs and "dobro" not in logs,
               "auditoria em disco: ações registradas, sem conteúdo de arquivo", logs[-400:])

        print("== F11. lia-chat: mostra a prévia e pede confirmação ==")
        chat = subprocess.Popen([CHAT], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                env={"HOME": home, "PATH": os.environ.get("PATH", "/usr/bin:/bin")})
        chat.stdin.write("crie o arquivo NOTAS.md com o conteúdo: # Notas\n")
        chat.stdin.flush()
        time.sleep(3)
        chat.stdin.write("s\n")
        chat.stdin.flush()
        time.sleep(3)
        chat.stdin.close()
        saida = chat.stdout.read()
        chat.wait(timeout=10)
        checar("[prévia] +# Notas" in saida and "Aprovar este plano? [s/N]" in saida, "F11: prévia e pergunta de aprovação no chat", saida[-2000:])
        checar(os.path.exists(os.path.join(proj, "NOTAS.md")) and "Feito: criar NOTAS.md" in saida, "F11: confirmado com 's' -> criado", saida[-1200:])
        parar(d)

        print("== F13. boas práticas: catálogo local, sem aprovação, extensão do projeto ==")
        os.makedirs(os.path.join(proj, ".lia"), exist_ok=True)
        with open(os.path.join(proj, ".lia", "praticas.json"), "w") as f:
            json.dump({"schema_version": 2, "praticas": [{
                "id": "projeto.mensagens-em-portugues", "versao": 1, "categoria": "git", "linguagens": ["*"], "ativacao": ["commit"],
                "regra": "Neste projeto, mensagens de commit em português.", "passos": ["escreva em português"], "checagens": ["mensagem em português"],
                "anti_padroes": [], "severidade": "obrigatoria", "prioridade": 100, "referencias": []}]}, f)
        parar(d)
        d = iniciar(home, proj)
        daemons.append(d)
        c = Conexao(home)
        pr = perguntar(c, "quais as boas práticas para tratar erros de malloc em C?")
        pl = pr.get("plano_proposto") or {}
        checar(pl.get("passos", [{}])[0].get("ferramenta") == "praticas.consultar" and pr["validacao"].get("execucao_automatica_permitida") is True,
               "F13: 'boas práticas' vira consulta ao catálogo local, sem aprovação (não vai para a web)", pr.get("validacao"))
        solicitar_plano(c, pr)
        msgs, _ = c.ate("resultado", 10)
        _, resp = c.ate("resposta", 10)
        ap = apresentacao_do_passo(msgs)
        checar("[c.dono-da-memoria]" in ap and "confira:" in ap and "orientação" in ap, "F13: práticas de C relevantes, com checagens", ap[:800])
        checar(resp and resp["dados"]["resposta_texto"].startswith("Selecionei"), "F13: resposta final curta", resp and resp["dados"])
        pr = perguntar(c, "boas práticas para fazer commit")
        solicitar_plano(c, pr)
        msgs, _ = c.ate("resultado", 10)
        c.ate("resposta", 10)
        ap = apresentacao_do_passo(msgs)
        checar("[projeto.mensagens-em-portugues]" in ap, "F13: prática do projeto (.lia/praticas.json) entra na seleção", ap[:600])
        c.fechar()
        log = open(os.path.join(home, "sessiond.log")).read()
        checar("boas práticas: 114 no catálogo" in log and "extensão do projeto" in log, "F13: catálogo + extensão carregados na inicialização",
               [l for l in log.splitlines() if "boas práticas" in l])

        print("== F12. sem projeto definido: ferramentas de arquivo indisponíveis ==")
        home2 = os.path.join(base, "home2")
        os.makedirs(home2)
        d2 = iniciar(home2, None)
        daemons.append(d2)
        c = Conexao(home2)
        pr = perguntar(c, "leia o arquivo src/main.c")
        checar(not pr.get("plano_proposto") and "projeto" in pr.get("resposta_texto", "").lower(), "F12: explica que não há projeto definido", pr)
        c.fechar()
        parar(d2)
        d3 = iniciar(os.path.join(base, "home2"), "/etc")
        daemons.append(d3)
        time.sleep(0.5)
        log = open(os.path.join(base, "home2", "sessiond.log")).read()
        checar("projeto RECUSADO" in log, "F12: /etc como projeto é recusado na inicialização", log[-300:])
        parar(d3)
    finally:
        for d in daemons:
            parar(d)
    restos = [p for p in subprocess.run(["pgrep", "-f", base], capture_output=True, text=True).stdout.split() if p != str(os.getpid())]
    checar(not restos, "nenhum processo residual", restos)
    if not os.environ.get("LIA_DEBUG_MANTER"):
        shutil.rmtree(base, ignore_errors=True)
    print("\n%d testes, %d falhas" % (total, falhas))
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
