#!/usr/bin/env python3
"""Testes do lia-chat num PSEUDO-TERMINAL de verdade (não pipe).

O lia-chat interativo desenha uma "região viva" (status + separador + linha
de entrada) e escreve o histórico acima dela (chat/terminal.c). Isso só
existe com um tty, então aqui o processo roda dentro de um pty e tudo que
ele escreve passa por um emulador VT mínimo (classe Tela, abaixo), que
mantém a grade de caracteres e a posição do cursor exatamente como um
terminal manteria. Os testes olham a TELA resultante — onde está o prompt,
quantas vezes a pergunta aparece, o que está acima do quê — e não apenas
os bytes emitidos.

Sempre em HOME temporário sob /tmp; nunca toca ~/.lia real; só ferramentas
de leitura (e uma de escrita, para provar que ela PARA em aprovação).

Uso: python3 testes/integration/teste_chat_pty.py   (make teste-chat)
"""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unicodedata

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SESSIOND = os.path.join(RAIZ, "build", "lia-sessiond")
CHAT = os.path.join(RAIZ, "build", "lia-chat")
FAKE_OLLAMA = os.path.join(RAIZ, "testes", "integration", "ollama_fake.py")

total = 0
falhas = 0


def ok(desc):
    global total
    total += 1
    print("ok   " + desc, flush=True)


def ko(desc, detalhe=""):
    global total, falhas
    total += 1
    falhas += 1
    print("FALHA " + desc + (("\n      " + detalhe.replace("\n", "\n      ")) if detalhe else ""), flush=True)


def checar(cond, desc, detalhe=""):
    if cond:
        ok(desc)
    else:
        ko(desc, detalhe)


# --------------------------------------------------------------------------
# Emulador VT mínimo: só o que chat/terminal.c emite.
# --------------------------------------------------------------------------
def largura(ch):
    if unicodedata.combining(ch) or unicodedata.category(ch) in ("Mn", "Me", "Cf"):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1


class Tela:
    def __init__(self, linhas=30, cols=100):
        self.linhas, self.cols = linhas, cols
        self.grade = [[" "] * cols for _ in range(linhas)]
        self.l = self.c = 0
        self.pendente = False       # cursor "pendurado" depois de escrever na última coluna
        self.salvo = (0, 0)
        self.rolado = []            # linhas que saíram por cima (scrollback)
        self.bruto = b""
        self.resto = b""
        self.colagem_ativa = False

    def redimensionar(self, linhas, cols):
        # terminal SEM reflow (como xterm): corta/estende, não redistribui
        nova = [[" "] * cols for _ in range(linhas)]
        for i in range(min(linhas, self.linhas)):
            for j in range(min(cols, self.cols)):
                nova[i][j] = self.grade[i][j]
        self.grade, self.linhas, self.cols = nova, linhas, cols
        self.l = min(self.l, linhas - 1)
        self.c = min(self.c, cols - 1)
        self.pendente = False

    def _rolar(self):
        self.rolado.append("".join(self.grade[0]).rstrip())
        self.grade.pop(0)
        self.grade.append([" "] * self.cols)

    def _nova_linha(self):
        if self.l == self.linhas - 1:
            self._rolar()
        else:
            self.l += 1

    def alimentar(self, dados):
        self.bruto += dados
        dados = self.resto + dados
        # não corta um caractere UTF-8 nem uma sequência de escape ao meio
        try:
            texto = dados.decode("utf-8")
            self.resto = b""
        except UnicodeDecodeError as e:
            texto = dados[:e.start].decode("utf-8", errors="replace")
            self.resto = dados[e.start:]
        i = 0
        n = len(texto)
        while i < n:
            ch = texto[i]
            if ch == "\x1b":
                if i + 1 >= n:
                    self.resto = texto[i:].encode() + self.resto
                    return
                prox = texto[i + 1]
                if prox == "[":
                    m = re.compile(r"\x1b\[([0-9;?]*)([@-~])").match(texto, i)
                    if not m:
                        self.resto = texto[i:].encode() + self.resto
                        return
                    self._csi(m.group(1), m.group(2))
                    i = m.end()
                    continue
                if prox == "7":
                    self.salvo = (self.l, self.c)
                elif prox == "8":
                    self.l, self.c = self.salvo
                    self.pendente = False
                i += 2
                continue
            if ch == "\r":
                self.c = 0
                self.pendente = False
            elif ch == "\n":
                self.pendente = False
                self._nova_linha()
            elif ch == "\b":
                self.c = max(0, self.c - 1)
                self.pendente = False
            elif ch == "\a" or ord(ch) < 0x20:
                pass
            else:
                w = largura(ch)
                if w == 0:
                    pass
                else:
                    if self.pendente:
                        self.c = 0
                        self._nova_linha()
                        self.pendente = False
                    self.grade[self.l][self.c] = ch
                    if w == 2 and self.c + 1 < self.cols:
                        self.grade[self.l][self.c + 1] = ""
                    if self.c + w >= self.cols:
                        self.c = self.cols - 1
                        self.pendente = True
                    else:
                        self.c += w
            i += 1

    def _csi(self, params, final):
        if params.startswith("?"):
            if params == "?2004":
                self.colagem_ativa = final == "h"
            return
        nums = [int(p) if p else 0 for p in params.split(";")] if params else []
        n1 = nums[0] if nums and nums[0] else 1
        self.pendente = False
        if final == "A":
            self.l = max(0, self.l - n1)
        elif final == "B":
            self.l = min(self.linhas - 1, self.l + n1)
        elif final == "C":
            self.c = min(self.cols - 1, self.c + n1)
        elif final == "D":
            self.c = max(0, self.c - n1)
        elif final == "H":
            self.l = (nums[0] - 1) if nums and nums[0] else 0
            self.c = (nums[1] - 1) if len(nums) > 1 and nums[1] else 0
        elif final == "J":
            modo = nums[0] if nums else 0
            if modo == 2:
                self.grade = [[" "] * self.cols for _ in range(self.linhas)]
            else:
                for j in range(self.c, self.cols):
                    self.grade[self.l][j] = " "
                for i in range(self.l + 1, self.linhas):
                    self.grade[i] = [" "] * self.cols
        elif final == "K":
            for j in range(self.c, self.cols):
                self.grade[self.l][j] = " "
        # 'm' (cores) e o resto: sem efeito na grade

    def linha(self, i):
        return "".join(self.grade[i]).rstrip()

    def visivel(self):
        return [self.linha(i) for i in range(self.linhas)]

    def tudo(self):
        """scrollback + tela visível, como texto"""
        return "\n".join(self.rolado + self.visivel())

    def ultima_nao_vazia(self):
        for i in range(self.linhas - 1, -1, -1):
            if self.linha(i):
                return i
        return -1

    def achar(self, trecho, desde=0):
        for i in range(desde, self.linhas):
            if trecho in self.linha(i):
                return i
        return -1

    def despejo(self):
        return "\n".join("%2d|%s" % (i, self.linha(i)) for i in range(self.linhas) if self.linha(i))


# --------------------------------------------------------------------------
# Processo num pty
# --------------------------------------------------------------------------
class ChatPty:
    def __init__(self, home, linhas=30, cols=100, env_extra=None, args=()):
        self.tela = Tela(linhas, cols)
        self.mestre, escravo = pty.openpty()
        self.escravo_attrs_iniciais = termios.tcgetattr(escravo)
        fcntl.ioctl(escravo, termios.TIOCSWINSZ, struct.pack("HHHH", linhas, cols, 0, 0))
        env = {"HOME": home, "TERM": "xterm-256color", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8",
               "PATH": os.environ.get("PATH", "/usr/bin:/bin")}
        if env_extra:
            env.update(env_extra)
        self.escravo = escravo
        self.proc = subprocess.Popen([CHAT] + list(args), stdin=escravo, stdout=escravo, stderr=escravo,
                                     env=env, start_new_session=True,
                                     preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))

    def bombear(self, segundos=0.15):
        fim = time.time() + segundos
        while True:
            resto = fim - time.time()
            r, _, _ = select.select([self.mestre], [], [], max(0, resto))
            if not r:
                break
            try:
                dados = os.read(self.mestre, 65536)
            except OSError:
                break
            if not dados:
                break
            self.tela.alimentar(dados)

    def esperar(self, cond, limite=10.0):
        fim = time.time() + limite
        while time.time() < fim:
            self.bombear(0.05)
            if cond(self.tela):
                self.bombear(0.15)  # deixa o quadro terminar de chegar
                return True
        return False

    def esperar_texto(self, trecho, limite=10.0):
        return self.esperar(lambda t: trecho in t.tudo(), limite)

    def teclar(self, dados):
        if isinstance(dados, str):
            dados = dados.encode()
        os.write(self.mestre, dados)
        self.bombear(0.15)

    def redimensionar(self, linhas, cols):
        fcntl.ioctl(self.mestre, termios.TIOCSWINSZ, struct.pack("HHHH", linhas, cols, 0, 0))
        self.tela.redimensionar(linhas, cols)
        try:
            os.killpg(self.proc.pid, signal.SIGWINCH)
        except ProcessLookupError:
            pass
        self.bombear(0.3)

    def sinal(self, sig):
        os.killpg(self.proc.pid, sig)
        self.bombear(0.2)

    def encerrar(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        for fd in (self.mestre, self.escravo):
            try:
                os.close(fd)
            except OSError:
                pass


def n_prompts(linhas):
    """linhas que são um prompt (a Tela tira o espaço final de 'lia> ' vazio)"""
    return sum(1 for ln in linhas if ln == "lia>" or ln.startswith("lia> "))


# região viva = 3 últimas linhas não vazias: status, separador, entrada
def regiao(t):
    u = t.ultima_nao_vazia()
    return u - 2, u - 1, u  # status, separador, entrada


def regiao_ok(t, entrada="lia> "):
    s, sep, e = regiao(t)
    if s < 0:
        return False
    return t.linha(e).rstrip() == entrada.rstrip() and set(t.linha(sep)) == {"─"} and t.linha(s) != ""


# --------------------------------------------------------------------------
# Ambiente
# --------------------------------------------------------------------------
def iniciar_daemon(home, env_extra=None):
    env = {"HOME": home, "PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LIA_SESSIOND_EXECUTOR": "real"}
    if os.environ.get("LIA_SESSIOND_SEM_RLIMIT_PARA_TESTE"):
        env["LIA_SESSIOND_SEM_RLIMIT_PARA_TESTE"] = "1"
    for k in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"):
        if os.environ.get(k):
            env[k] = os.environ[k]
    if env_extra:
        env.update(env_extra)
    log = open(os.path.join(home, "sessiond.log"), "w")
    p = subprocess.Popen([SESSIOND], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    sock = os.path.join(home, ".lia", "run", "session.sock")
    for _ in range(100):
        if os.path.exists(sock):
            break
        time.sleep(0.05)
    return p


def parar(p):
    if p and p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()


def iniciar_fake_ollama(home, modo):
    arq = os.path.join(home, "fake.port")
    saida = open(arq, "w")
    env = dict(os.environ, OLLAMA_FAKE_MODO=modo)
    p = subprocess.Popen([sys.executable, FAKE_OLLAMA, "0"], env=env, stdout=saida, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    for _ in range(100):
        if os.path.getsize(arq) > 0:
            break
        time.sleep(0.05)
    return p, open(arq).read().split()[0]


PERGUNTA_MEM = "liste pra mim os processos que mais consumiram memoria ram"


def main():
    if not (os.access(SESSIOND, os.X_OK) and os.access(CHAT, os.X_OK)):
        print("lia-sessiond/lia-chat não encontrados — rode 'make teste-chat'")
        return 1
    base = tempfile.mkdtemp(prefix="lia-pty-", dir="/tmp")
    home_real = os.path.expanduser("~")
    assert base.startswith("/tmp/") and base != home_real
    home_a = os.path.join(base, "a")
    home_b = os.path.join(base, "b")
    os.makedirs(home_a)
    os.makedirs(home_b)
    print("ambiente isolado: %s (HOME real intocado: %s)" % (base, home_real))
    marca = time.time()
    daemon_a = daemon_b = fake = None
    chats = []
    chats_daemons = []

    def novo_chat(home, **kw):
        ch = ChatPty(home, **kw)
        chats.append(ch)
        return ch

    try:
        # daemon A: executor real + modelo mock (instantâneo, determinístico)
        daemon_a = iniciar_daemon(home_a)

        print("== P1. região viva: status, separador e UM prompt, no fim da tela ==")
        ch = novo_chat(home_a)
        checar(ch.esperar(regiao_ok, 5), "P1: status + separador + 'lia> ' vazio são as três últimas linhas da tela", ch.tela.despejo())
        s, sep, e = regiao(ch.tela)
        checar("pronto" in ch.tela.linha(s) and "leituras seguras automáticas" in ch.tela.linha(s),
               "P1: linha de status mostra o estado operacional (conectado/pronto/piloto automático)", ch.tela.linha(s))
        checar((ch.tela.l, ch.tela.c) == (e, len("lia> ")), "P1: cursor visível, logo depois do prompt",
               "cursor=%r esperado=%r" % ((ch.tela.l, ch.tela.c), (e, 5)))
        checar(n_prompts(ch.tela.rolado + ch.tela.visivel()) == 1, "P1: exatamente um prompt na tela")
        checar(ch.tela.colagem_ativa, "P1: colagem delimitada (bracketed paste) ligada")
        checar("\x1b[?25l" not in ch.tela.bruto.decode("utf-8", "replace"), "P1: o cursor nunca é escondido")

        print("== P2. edição: acentos, backspace, setas, Home/End, Ctrl-A/E/K/U/W ==")
        ch.teclar("ação é útil")
        s, sep, e = regiao(ch.tela)
        checar(ch.tela.linha(e) == "lia> ação é útil", "P2: texto com acentos aparece inteiro (UTF-8)", repr(ch.tela.linha(e)))
        ch.teclar("\x7f\x7f\x7f")
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> ação é ú", "P2: backspace apaga um CARACTERE por vez (não um byte)", repr(ch.tela.linha(regiao(ch.tela)[2])))
        ch.teclar("\x1b[D\x1b[D\x1b[D\x1b[D")      # 4x seta esquerda
        ch.teclar("X")
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> açãoX é ú", "P2: seta esquerda move o cursor; inserção no meio", repr(ch.tela.linha(regiao(ch.tela)[2])))
        ch.teclar("\x1b[H")                           # Home
        ch.teclar(">")
        ch.teclar("\x1b[F")                           # End
        ch.teclar("<")
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> >açãoX é ú<", "P2: Home e End", repr(ch.tela.linha(regiao(ch.tela)[2])))
        ch.teclar("\x01\x1b[C\x1b[3~")               # Ctrl-A, direita, Delete
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> >çãoX é ú<", "P2: Delete apaga o caractere sob o cursor", repr(ch.tela.linha(regiao(ch.tela)[2])))
        ch.teclar("\x05\x17")                         # Ctrl-E, Ctrl-W
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> >çãoX é", "P2: Ctrl-W apaga a palavra anterior", repr(ch.tela.linha(regiao(ch.tela)[2])))
        ch.teclar("\x01\x1b[C\x0b")                  # Ctrl-A, direita, Ctrl-K
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> >", "P2: Ctrl-K apaga até o fim da linha", repr(ch.tela.linha(regiao(ch.tela)[2])))
        ch.teclar("\x15")                             # Ctrl-U
        checar(regiao_ok(ch.tela), "P2: Ctrl-U limpa a linha; região viva intacta", ch.tela.despejo())
        checar(n_prompts(ch.tela.rolado + ch.tela.visivel()) == 1, "P2: nenhuma cópia extra do prompt depois de toda a edição")

        print("== P3. pergunta de memória: ferramenta certa -> dados reais -> resposta final, tudo ACIMA da entrada ==")
        ch.teclar(PERGUNTA_MEM)
        ch.teclar("\r")
        fim = ch.esperar(lambda t: "concluído em" in t.tudo() and regiao_ok(t), 15)
        checar(fim, "P3: o turno termina sozinho (sem /plano) e a entrada volta vazia", ch.tela.despejo())
        tudo = ch.tela.tudo()
        checar(tudo.count(PERGUNTA_MEM) == 1, "P3: a pergunta aparece UMA vez (eco no histórico) — nunca duplicada",
               "ocorrências=%d" % tudo.count(PERGUNTA_MEM))
        s, sep, e = regiao(ch.tela)
        checar(PERGUNTA_MEM not in ch.tela.linha(e), "P3: a linha de entrada não contém a pergunta já enviada", repr(ch.tela.linha(e)))
        linhas = ch.tela.rolado + ch.tela.visivel()

        def pos(trecho):
            for i, ln in enumerate(linhas):
                if trecho in ln:
                    return i
            return -1
        p_eco, p_int, p_fer = pos("lia> " + PERGUNTA_MEM), pos("intenção: processos por uso de memória RAM"), pos("ferramenta: sistema.processos_top_memoria")
        p_dados, p_tab, p_lia, p_fim = pos("dados reais recebidos de sistema.processos_top_memoria"), pos("RSS (MiB)"), pos("Lia:"), pos("concluído em")
        p_status = len(ch.tela.rolado) + s
        ordem = [p_eco, p_int, p_fer, p_dados, p_tab, p_lia, p_fim, p_status]
        checar(all(x >= 0 for x in ordem) and ordem == sorted(ordem),
               "P3: ordem na tela: pergunta < intenção < ferramenta < dados reais < tabela < resposta < concluído < status/entrada",
               "posições=%r\n%s" % (ordem, ch.tela.despejo()))
        checar("sistema.recursos" not in tudo and "sistema.dispositivos" not in tudo,
               "P3: nem sistema.recursos nem sistema.dispositivos aparecem numa pergunta de processos")
        # dado real: o 1º da tabela precisa existir em /proc com o mesmo nome
        m = None
        for ln in linhas[p_tab + 1:]:
            m = re.match(r"\s+1\s+(\d+)\s+(.+?)\s{2,}\S+\s+(\d+)\s+[\d,]+\s*$", ln)
            if m:
                break
        real = False
        if m:
            try:
                real = open("/proc/%s/comm" % m.group(1)).read().strip() == m.group(2).strip()
            except OSError:
                real = False
        checar(real, "P3: o primeiro processo da tabela existe de verdade em /proc com esse PID e nome (dado real, não inventado)",
               "linha=%r" % (m.group(0) if m else None))
        checar("modelo/proposta" in tudo or "ferramenta" in linhas[p_fim], "P3: tempos reais de cada etapa mostrados ao concluir", linhas[p_fim])

        print("== P4. histórico: ↑ recupera, ↓ volta ao rascunho ==")
        ch.teclar("rascunho")
        ch.teclar("\x1b[A")
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> " + PERGUNTA_MEM, "P4: ↑ recupera a última linha enviada", repr(ch.tela.linha(regiao(ch.tela)[2])))
        ch.teclar("\x1b[B")
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> rascunho", "P4: ↓ volta ao que estava sendo digitado", repr(ch.tela.linha(regiao(ch.tela)[2])))
        ch.teclar("\x15")

        print("== P5. colagem: quebra de linha colada NUNCA envia; vira espaço ==")
        antes = ch.tela.tudo().count("consultando") + ch.tela.tudo().count("intenção:")
        ch.teclar("\x1b[200~linha um\nlinha dois\r\nlinha três\x1b[201~")
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> linha um linha dois  linha três",
               "P5: texto colado com quebras fica numa linha só de entrada", repr(ch.tela.linha(regiao(ch.tela)[2])))
        depois = ch.tela.tudo().count("consultando") + ch.tela.tudo().count("intenção:")
        checar(antes == depois and regiao(ch.tela)[2] == ch.tela.ultima_nao_vazia(), "P5: nada foi enviado ao colar")
        ch.teclar("\x15")

        print("== P6. linha de entrada mais longa que o terminal ocupa UMA linha (janela em volta do cursor) ==")
        u_antes = ch.tela.ultima_nao_vazia()
        ch.teclar("palavra " * 30)
        s, sep, e = regiao(ch.tela)
        checar(ch.tela.ultima_nao_vazia() == u_antes and ch.tela.linha(e).startswith("lia> …") and set(ch.tela.linha(sep)) == {"─"},
               "P6: entrada longa não quebra em várias linhas nem empurra a região", ch.tela.despejo())
        ch.teclar("\x01")
        checar(ch.tela.linha(regiao(ch.tela)[2]).startswith("lia> palavra palavra") and ch.tela.linha(regiao(ch.tela)[2]).endswith("…"),
               "P6: Ctrl-A volta ao começo da linha longa", repr(ch.tela.linha(regiao(ch.tela)[2])))
        ch.teclar("\x0b")

        print("== P7. resposta longa (mais linhas que a tela): região viva continua no fim, uma só ==")
        ch.teclar("Quais dispositivos PCI existem?\r")
        fim = ch.esperar(lambda t: t.tudo().count("concluído em") >= 2 and regiao_ok(t), 15)
        checar(fim, "P7: turno com saída longa termina com a região viva íntegra", ch.tela.despejo())
        n_pci = len([x for x in os.listdir("/sys/bus/pci/devices")])
        checar("Dispositivos PCI (%d)" % n_pci in ch.tela.tudo(), "P7: contagem de dispositivos PCI igual à de /sys/bus/pci/devices (%d)" % n_pci)
        checar(len(ch.tela.rolado) > 0, "P7: o histórico rolou para o scrollback normal do terminal (nada de tela cheia)")
        checar(n_prompts(ch.tela.visivel()) == 1 and sum(1 for ln in ch.tela.visivel() if ln and set(ln) == {"─"}) == 1,
               "P7: depois de rolar, só UM prompt e UM separador visíveis", ch.tela.despejo())

        print("== P8. redimensionar: região redesenhada, sem prompt duplicado, e continua funcionando ==")
        ch.teclar("meio digitado")
        ch.redimensionar(30, 70)
        vis = ch.tela.visivel()
        checar(n_prompts(vis) == 1 and ch.tela.linha(regiao(ch.tela)[2]) == "lia> meio digitado",
               "P8: depois de encolher para 70 colunas: um prompt, com o texto digitado preservado", ch.tela.despejo())
        ch.redimensionar(36, 120)
        checar(n_prompts(ch.tela.visivel()) == 1 and ch.tela.linha(regiao(ch.tela)[2]) == "lia> meio digitado",
               "P8: depois de crescer para 120 colunas: idem", ch.tela.despejo())
        checar((ch.tela.l, ch.tela.c) == (regiao(ch.tela)[2], len("lia> meio digitado")), "P8: cursor no fim do texto digitado")
        ch.teclar("\x15Qual kernel e distribuição estão em execução?\r")
        fim = ch.esperar(lambda t: t.tudo().count("concluído em") >= 3 and regiao_ok(t), 15)
        kernel = os.uname().release
        checar(fim and kernel in ch.tela.tudo(), "P8: pergunta após redimensionar funciona e mostra o kernel real (%s)" % kernel, ch.tela.despejo())

        print("== P9. ferramenta de ESCRITA nunca roda sozinha: para em aprovação; padrão é rejeitar ==")
        ch.teclar("MOCK_PROPOR:tools.discover.persistir\r")
        checar(ch.esperar_texto("não roda sozinha", 8), "P9: proposta de ferramenta que grava em disco não é auto-executada", ch.tela.despejo())
        checar("exige aprovação" in ch.tela.tudo(), "P9: o motivo é mostrado (exige aprovação)")
        ch.teclar("/plano\r")
        aprov = ch.esperar(lambda t: t.linha(t.ultima_nao_vazia()).startswith("aprovar? [s/N]"), 8)
        checar(aprov, "P9: /plano mostra o plano e o prompt vira 'aprovar? [s/N]'", ch.tela.despejo())
        checar("Requer aprovação: sim" in ch.tela.tudo() and "aguardando aprovação" in ch.tela.linha(regiao(ch.tela)[0]),
               "P9: plano completo (risco/efeitos) exibido e status 'aguardando aprovação'", ch.tela.despejo())
        ch.teclar("\r")   # linha vazia: não decide nada
        ch.teclar("n\r")
        checar(ch.esperar(lambda t: "rejeição enviado" in t.tudo() and regiao_ok(t), 8), "P9: 'n' rejeita; prompt normal de volta", ch.tela.despejo())
        inv = os.path.join(home_a, ".lia", "maquina", "inventario", "ferramentas.lia")
        checar(not os.path.exists(inv), "P9: nada foi gravado em disco")

        print("== P10. /sair: encerra limpo e devolve o terminal como estava ==")
        ch.teclar("/sair\r")
        try:
            codigo = ch.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            codigo = None
        ch.bombear(0.3)
        checar(codigo == 0, "P10: /sair -> código de saída 0", "codigo=%r" % codigo)
        attrs = termios.tcgetattr(ch.escravo)
        checar(attrs[3] == ch.escravo_attrs_iniciais[3], "P10: modo do terminal (eco, canônico) restaurado ao sair",
               "lflag=%o esperado=%o" % (attrs[3], ch.escravo_attrs_iniciais[3]))
        checar(not ch.tela.colagem_ativa, "P10: colagem delimitada desligada ao sair")
        ch.encerrar()

        # daemon B: modelo LENTO (servidor Ollama falso, ~6 s por resposta)
        fake, porta = iniciar_fake_ollama(home_b, "lento")
        daemon_b = iniciar_daemon(home_b, {"LIA_SESSIOND_MODELO": "ollama", "LIA_OLLAMA_PORTA": porta})

        print("== P11. status ao vivo + saída assíncrona enquanto se digita ==")
        ch = novo_chat(home_b)
        ch.esperar(regiao_ok, 5)
        ch.teclar("oi\r")
        checar(ch.esperar(lambda t: "consultando modelo" in t.linha(regiao(t)[0]), 5), "P11: status passa a 'consultando modelo'", ch.tela.despejo())
        st1 = ch.tela.linha(regiao(ch.tela)[0])
        ch.teclar("rascunho çã")     # digitando enquanto o modelo trabalha
        ch.bombear(2.3)
        st2 = ch.tela.linha(regiao(ch.tela)[0])
        seg = lambda x: int(re.search(r"(\d+)s", x).group(1)) if re.search(r"(\d+)s", x) else -1
        checar(seg(st2) > seg(st1) >= 0, "P11: o tempo decorrido na linha de status avança ao vivo", "%r -> %r" % (st1, st2))
        checar("Ctrl-C cancela" in st2, "P11: a linha de status diz como cancelar")
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> rascunho çã", "P11: o que está sendo digitado continua na entrada durante a espera", ch.tela.despejo())
        bruto = ch.tela.bruto.decode("utf-8", "replace")
        checar(len(set(re.findall(r"[⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏]", bruto))) >= 3, "P11: indicador de atividade anima (vários quadros)")
        chegou = ch.esperar(lambda t: "Lia: ola mundo" in t.tudo(), 15)
        checar(chegou, "P11: a resposta chega", ch.tela.despejo())
        s, sep, e = regiao(ch.tela)
        p_resp = ch.tela.achar("Lia: ola mundo")
        checar(ch.tela.linha(e) == "lia> rascunho çã" and 0 <= p_resp < s,
               "P11: resposta entrou ACIMA da região viva; texto digitado e prompt preservados", ch.tela.despejo())
        checar((ch.tela.l, ch.tela.c) == (e, len("lia> rascunho çã")), "P11: cursor continua no fim do texto digitado")
        checar(ch.tela.tudo().count("lia> rascunho çã") == 1, "P11: o rascunho não foi duplicado pela saída assíncrona")
        checar("pronto" in ch.tela.linha(s), "P11: status volta a 'pronto' ao concluir")

        print("== P12. Ctrl-C cancela a pergunta em andamento; a sessão continua ==")
        ch.teclar("\x15pergunta que vou cancelar\r")
        ch.esperar(lambda t: "consultando modelo" in t.linha(regiao(t)[0]), 5)
        ch.teclar("nao perca isto")
        ch.sinal(signal.SIGINT)
        canc = ch.esperar(lambda t: "cancelado" in t.tudo() and "pronto" in t.linha(regiao(t)[0]), 5)
        checar(canc, "P12: Ctrl-C cancela a consulta ao modelo e o status volta a 'pronto'", ch.tela.despejo())
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> nao perca isto", "P12: Ctrl-C preserva o texto que estava sendo digitado", ch.tela.despejo())
        ch.bombear(7.0)
        checar("Lia: ola mundo" not in "\n".join((ch.tela.rolado + ch.tela.visivel())[ch.tela.achar("pergunta que vou cancelar"):] if ch.tela.achar("pergunta que vou cancelar") >= 0 else []),
               "P12: nenhuma resposta atrasada aparece depois do cancelamento")
        checar(ch.proc.poll() is None, "P12: um Ctrl-C não encerra o chat")
        log_b = open(os.path.join(home_b, "sessiond.log")).read()
        checar("pergunta ao modelo cancelada pelo cliente" in log_b, "P12: o daemon registrou o cancelamento de verdade (geração interrompida)")

        print("== P13. encerrar durante o streaming: sai na hora, daemon cancela o modelo e segue vivo ==")
        ch.teclar("\x15outra pergunta lenta\r")
        ch.esperar(lambda t: "consultando modelo" in t.linha(regiao(t)[0]), 5)
        t0 = time.time()
        ch.teclar("/sair\r")
        try:
            codigo = ch.proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            codigo = None
        checar(codigo == 0 and time.time() - t0 < 3, "P13: /sair no meio da resposta encerra imediatamente (código 0)", "codigo=%r" % codigo)
        time.sleep(0.5)
        checar(daemon_b.poll() is None, "P13: o daemon continua vivo depois que o cliente sai no meio do streaming")
        ch.encerrar()
        ch = novo_chat(home_b)
        checar(ch.esperar(regiao_ok, 5), "P13: uma nova sessão conecta normalmente em seguida")
        ch.sinal(signal.SIGINT)
        ch.esperar_texto("nada para cancelar", 3)
        ch.sinal(signal.SIGINT)
        try:
            codigo = ch.proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            codigo = None
        checar(codigo == 130, "P13: Ctrl-C duas vezes com a sessão ociosa encerra com código 130", "codigo=%r" % codigo)
        attrs = termios.tcgetattr(ch.escravo)
        checar(attrs[3] == ch.escravo_attrs_iniciais[3], "P13: terminal restaurado também na saída por Ctrl-C")
        ch.encerrar()

        print("== P14. modo manual e valor desconhecido de LIA_CHAT_AUTOPILOT ==")
        ch = novo_chat(home_a, env_extra={"LIA_CHAT_AUTOPILOT": "manual"})
        ch.esperar(regiao_ok, 5)
        checar("modo manual" in ch.tela.linha(regiao(ch.tela)[0]), "P14: status indica modo manual")
        ch.teclar(PERGUNTA_MEM + "\r")
        checar(ch.esperar_texto("não roda sozinha (modo manual)", 8), "P14: em manual, mesmo a leitura segura espera /plano", ch.tela.despejo())
        ch.bombear(1.0)
        checar("dados reais recebidos" not in ch.tela.tudo(), "P14: nada executou sem confirmação em modo manual")
        ch.teclar("/plano\r")
        checar(ch.esperar(lambda t: "concluído em" in t.tudo() and regiao_ok(t), 15), "P14: /plano executa e o turno termina com resposta final", ch.tela.despejo())
        ch.teclar("/sair\r")
        ch.proc.wait(timeout=5)
        ch.encerrar()
        r = subprocess.run([CHAT], env={"HOME": home_a, "LIA_CHAT_AUTOPILOT": "tudo", "PATH": os.environ.get("PATH", "")},
                           stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
        checar(r.returncode == 2 and b"Recusando iniciar" in r.stderr, "P14: LIA_CHAT_AUTOPILOT desconhecido -> recusa iniciar (código 2)", r.stderr.decode())
        r = subprocess.run([CHAT], env={"HOME": home_a, "LIA_CHAT_AUTOPILOT": "admin", "PATH": os.environ.get("PATH", "")},
                           stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
        checar(r.returncode == 2, "P14: não existe valor de ambiente que libere ferramenta administrativa")

        print("== P16. TAREFA de vários passos: plano, passos, verificação, relatório — tudo acima da entrada ==")
        ch = novo_chat(home_a, linhas=60, cols=118)
        ch.esperar(regiao_ok, 5)
        PERG = "Analise por que o sistema pode estar lento agora."
        ch.teclar(PERG + "\r")
        fim = ch.esperar(lambda t: "concluído em" in t.tudo() and regiao_ok(t), 20)
        checar(fim, "P16: a tarefa termina sozinha e a entrada volta vazia", ch.tela.despejo())
        linhas = ch.tela.rolado + ch.tela.visivel()
        tudo = "\n".join(linhas)

        def pos(trecho, desde=0):
            for i in range(desde, len(linhas)):
                if trecho in linhas[i]:
                    return i
            return -1
        checar(tudo.count(PERG) == 1, "P16: a pergunta aparece uma vez")
        checar("Plano da tarefa" in tudo and "4 passos" in tudo and "somente leitura, sem aprovação" in tudo and re.search(r"hash [0-9a-f]{12}…", tudo),
               "P16: plano mostrado com nº de passos, risco, orçamento e o hash a que a execução fica presa")
        passos = [pos("[%d/4] %s" % (i, f)) for i, f in ((1, "sistema.recursos"), (2, "sistema.processos_top_cpu"), (3, "sistema.processos_top_memoria"), (4, "sistema.dispositivos"))]
        checar(all(x >= 0 for x in passos) and tudo.count("dados reais recebidos") == 4 and tudo.count("evidências:") >= 4,
               "P16: os quatro passos aparecem, cada um com dados reais e evidências", passos)
        ordem = [pos("lia> " + PERG), pos("Plano da tarefa"), passos[0], pos("Passos:"), pos("verificação: todos os critérios têm evidência"),
                 pos("Lia:"), pos("Evidências"), pos("Limitações:"), pos("Ações realizadas"), pos("Ações não realizadas"), pos("Próximo passo:"),
                 pos("concluído em"), len(ch.tela.rolado) + regiao(ch.tela)[0]]
        checar(all(x >= 0 for x in ordem) and ordem == sorted(ordem),
               "P16: ordem na tela: pergunta < plano < passos < verificação < conclusão < evidências < limitações < ações < próximo passo < status/entrada", ordem)
        checar("Conclusão:" in linhas[pos("Lia:")], "P16: a resposta final começa pela conclusão")
        mem_total = int(next(l for l in open("/proc/meminfo") if l.startswith("MemTotal:")).split()[1]) // 1024
        checar("de %d MiB" % mem_total in tudo, "P16: o relatório cita a RAM total real (%d MiB)" % mem_total)
        s_, sep_, e_ = regiao(ch.tela)
        checar("Objetivo:" not in ch.tela.linha(s_ - 1) and n_prompts(ch.tela.visivel()[max(0, s_):]) == 1, "P16: o painel da tarefa some ao concluir; resta só a região normal")
        checar("auditoria registrada" not in tudo, "P16: sem ruído de auditoria por passo na tela")

        print("== P17. pergunta de vírus: resposta direta, nada executado ==")
        antes = ch.tela.tudo().count("dados reais recebidos")
        ch.teclar("Existe vírus nesta máquina?\r")
        okv = ch.esperar(lambda t: t.tudo().count("concluído em") >= 2 and regiao_ok(t), 10)
        tudo = ch.tela.tudo()
        checar(okv and "Não há ferramenta de detecção de malware" in tudo and "ferramenta inexistente" in tudo, "P17: diz que não há ferramenta de detecção (compatibilidade: inexistente)", ch.tela.despejo())
        checar(tudo.count("dados reais recebidos") == antes and "tools.discover" not in tudo, "P17: nenhuma ferramenta foi executada; tools.discover não aparece")
        ch.teclar("/sair\r")
        ch.proc.wait(timeout=5)
        ch.encerrar()

        print("== P18. painel vivo da tarefa + cancelamento GLOBAL com Ctrl-C ==")
        home_c = os.path.join(base, "c")
        os.makedirs(home_c)
        daemon_c = iniciar_daemon(home_c, {"LIA_SESSIOND_EXECUTOR": "mock"})
        chats_daemons.append(daemon_c)
        ch = novo_chat(home_c, linhas=40, cols=118)
        ch.esperar(regiao_ok, 5)
        ch.teclar('MOCK_PLANO:{"objetivo":"esperar muito","passos":[{"ferramenta":"mock.timeout","timeout_ms":20000},'
                  '{"ferramenta":"mock.timeout","timeout_ms":20000},{"ferramenta":"mock.leitura","depende_de":["passo-1"]}]}\r')
        vivo = ch.esperar(lambda t: "coletando dados" in t.linha(regiao(t)[0]), 8)
        s_, sep_, e_ = regiao(ch.tela)
        painel = [ch.tela.linha(i) for i in range(max(0, s_ - 4), s_)]
        checar(vivo and painel[0].startswith("Objetivo: esperar muito") and "[1/3] mock.timeout" in painel[1] and "executando" in painel[1] and
               "executando" in painel[2] and "[3/3] mock.leitura" in painel[3] and "aguardando" in painel[3],
               "P18: painel da tarefa (objetivo + estado de cada passo) fica logo ACIMA da linha de status, atualizado ao vivo", "\n".join(painel))
        checar("0/3 passos" in ch.tela.linha(s_) and "mock.timeout" in ch.tela.linha(s_) and "Ctrl-C cancela" in ch.tela.linha(s_),
               "P18: status mostra a fase (coletando dados), progresso, ferramenta atual e como cancelar", ch.tela.linha(s_))
        ch.teclar("rascunho")
        checar(ch.tela.linha(regiao(ch.tela)[2]) == "lia> rascunho" and "Objetivo:" in ch.tela.linha(regiao(ch.tela)[0] - 4),
               "P18: digitar durante a tarefa não bagunça o painel", ch.tela.despejo())
        t0 = time.time()
        ch.sinal(signal.SIGINT)
        canc = ch.esperar(lambda t: "tarefa cancelada" in t.tudo() and "pronto" in t.linha(regiao(t)[0]), 5)
        checar(canc and time.time() - t0 < 2.5, "P18: um Ctrl-C cancela a tarefa INTEIRA de imediato", ch.tela.despejo())
        tudo = ch.tela.tudo()
        checar(tudo.count("cancelado") >= 2 and "pulado" in tudo, "P18: os dois passos em execução aparecem como cancelados e o pendente como pulado")
        s_, sep_, e_ = regiao(ch.tela)
        checar(ch.tela.linha(e_) == "lia> rascunho" and not ch.tela.linha(s_ - 1).startswith("  ["), "P18: painel removido; o rascunho continua na entrada", ch.tela.despejo())
        ch.bombear(1.0)
        checar("dados reais recebidos" not in ch.tela.tudo() and ch.proc.poll() is None, "P18: nenhum passo continua depois do cancelamento; o chat segue vivo")

        print("== P19. tarefa com passo de ESCRITA: nunca automática; aprovação explícita presa ao hash ==")
        ch.teclar('\x15MOCK_PLANO:{"objetivo":"gravar algo","passos":[{"ferramenta":"mock.leitura"},{"ferramenta":"mock.escrita"}]}\r')
        checar(ch.esperar_texto("a tarefa não roda sozinha (exige aprovação)", 8), "P19: tarefa com escrita não é auto-executada", ch.tela.despejo())
        ch.bombear(0.8)
        checar("[1/2] mock.leitura — dados reais" not in ch.tela.tudo(), "P19: nem o passo de leitura rodou antes da aprovação")
        ch.teclar("/plano\r")
        ap = ch.esperar(lambda t: t.linha(t.ultima_nao_vazia()).startswith("aprovar? [s/N]"), 8)
        checar(ap and "EXIGE APROVAÇÃO" in ch.tela.tudo() and "risco medio" in ch.tela.tudo(), "P19: /plano mostra o plano (risco médio, EXIGE APROVAÇÃO) e pergunta", ch.tela.despejo())
        ch.teclar("s\r")
        fim = ch.esperar(lambda t: "verificação: todos os critérios têm evidência" in t.tudo() and "concluído em" in t.tudo().split("gravar algo")[-1] and regiao_ok(t), 12)
        checar(fim and "[2/2] mock.escrita — dados reais recebidos" in ch.tela.tudo(), "P19: aprovada com 's', a tarefa executa os dois passos e conclui", ch.tela.despejo())
        ch.teclar("/sair\r")
        ch.proc.wait(timeout=5)
        ch.encerrar()

        print("== P20. tarefa de CÓDIGO: a linha de status mostra a etapa real — nunca 'coletando dados · 0/0 passos' ==")
        home_c = os.path.join(base, "cod")
        proj_c = os.path.join(base, "projc")
        os.makedirs(home_c)
        os.makedirs(proj_c)
        with open(os.path.join(proj_c, "soma.c"), "w") as f:
            f.write("int soma(int a, int b) { return a - b; }\n")
        with open(os.path.join(proj_c, "Makefile"), "w") as f:
            f.write("teste:\n\ttrue\n")
        daemon_c = iniciar_daemon(home_c, {"LIA_SESSIOND_PROJETO": proj_c, "LIA_SESSIOND_INTERVALO_PROGRESSO_MS": "500"})
        chats_daemons.append(daemon_c)
        ch = novo_chat(home_c)
        ch.esperar(regiao_ok, 5)
        ch.teclar("corrija o bug na função soma em soma.c MOCK_CODIGO_LENTO\r")
        vistos = []

        def status_codigo(t):
            st = t.linha(regiao(t)[0])
            vistos.append(st)
            return "código · proposta · ciclo 1/3" in st

        checar(ch.esperar(status_codigo, 8), "P20: status mostra 'código · proposta · ciclo 1/3' enquanto o modelo escreve", vistos[-3:])
        checar(not any("0/0" in v or "coletando dados" in v for v in vistos), "P20: nunca 'coletando dados · 0/0 passos' durante o ciclo de código", vistos)
        checar(ch.esperar(lambda t: "aguardando o modelo local" in t.tudo(), 5), "P20: batida de progresso aparece enquanto o modelo trabalha", ch.tela.despejo())
        ch.teclar("\x03")
        checar(ch.esperar(lambda t: "pronto" in t.linha(regiao(t)[0]), 8), "P20: Ctrl-C cancela a tarefa de código e a sessão volta a 'pronto'", ch.tela.despejo())
        ch.teclar("/sair\r")
        ch.proc.wait(timeout=5)
        ch.encerrar()

        print("== P15. isolamento ==")
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
        checar(not alterado, "P15: nada em ~/.lia real foi criado/alterado")
    finally:
        for ch in chats:
            ch.encerrar()
        parar(daemon_a)
        parar(daemon_b)
        parar(fake)
        for dd in chats_daemons:
            parar(dd)

    restos = subprocess.run(["pgrep", "-f", base], capture_output=True, text=True).stdout.split()
    restos = [p for p in restos if p != str(os.getpid())]
    checar(not restos, "P15: nenhum processo residual referenciando o HOME temporário", "pids=%r" % restos)
    socks = [os.path.join(h, ".lia", "run", "session.sock") for h in (home_a, home_b, os.path.join(base, "c"))]
    checar(not any(os.path.exists(x) for x in socks), "P15: sockets removidos ao encerrar os daemons")
    if not os.environ.get("LIA_DEBUG_MANTER"):
        subprocess.run(["rm", "-rf", base])

    print("\n%d testes, %d falhas" % (total, falhas))
    return 1 if falhas else 0


if __name__ == "__main__":
    sys.exit(main())
