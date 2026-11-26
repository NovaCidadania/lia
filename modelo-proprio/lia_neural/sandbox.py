"""Executa código proposto por um modelo (NÃO confiável) num subprocesso isolado: bwrap sem rede/sem home quando disponível,
sempre com limites de CPU/memória/tempo e sem herdar o ambiente. Devolve o resultado de cada entrada."""
import json, os, resource, shutil, subprocess, sys

_FILHO = r'''
import sys, json, signal
d = json.loads(sys.stdin.read())
def _h(*a): raise TimeoutError()
signal.signal(signal.SIGALRM, _h)
saida, err, f = [], None, None
try:
    ns = {}
    exec(d["codigo"], ns)
    f = ns[d["funcao"]]
except BaseException as e:
    err = type(e).__name__
for args in d["entradas"]:
    if err:
        saida.append(["erro", err]); continue
    signal.alarm(1)
    try:
        v = f(*args)
        try:
            v = json.loads(json.dumps(v))
        except Exception:
            v = repr(v)
        saida.append(["ok", v])
    except BaseException as e:
        saida.append(["erro", type(e).__name__])
    finally:
        signal.alarm(0)
sys.__stdout__.write("\n@@RES@@" + json.dumps(saida) + "\n")
'''
_PY = "/usr/bin/python3"


def _limites():
    resource.setrlimit(resource.RLIMIT_CPU, (6, 6))
    resource.setrlimit(resource.RLIMIT_AS, (768 * 1024 * 1024, 768 * 1024 * 1024))
    resource.setrlimit(resource.RLIMIT_FSIZE, (1 << 20, 1 << 20))
    os.setsid()


def rodar_casos(codigo, funcao, entradas, limite_s=10):
    """-> lista de ("ok", valor) | ("erro", Tipo); em falha do próprio sandbox, tudo vira ("erro", "sandbox")"""
    cmd = [_PY, "-I", "-c", _FILHO]
    if shutil.which("bwrap"):
        cmd = ["bwrap", "--unshare-all", "--die-with-parent", "--ro-bind", "/usr", "/usr", "--symlink", "usr/bin", "/bin",
               "--symlink", "usr/lib", "/lib", "--symlink", "usr/lib", "/lib64", "--proc", "/proc", "--dev", "/dev",
               "--tmpfs", "/tmp", "--chdir", "/tmp"] + cmd
    falha = [("erro", "sandbox")] * len(entradas)
    try:
        p = subprocess.run(cmd, input=json.dumps({"codigo": codigo, "funcao": funcao, "entradas": entradas}), capture_output=True,
                           text=True, timeout=limite_s, preexec_fn=_limites, env={"PATH": "/usr/bin"})
    except subprocess.TimeoutExpired:
        return [("erro", "Tempo")] * len(entradas)
    for linha in reversed(p.stdout.splitlines()):
        if linha.startswith("@@RES@@"):
            try:
                r = [tuple(x) for x in json.loads(linha[7:])]
                return r if len(r) == len(entradas) else falha
            except ValueError:
                return falha
    return falha
