#!/usr/bin/env python3
"""Servidor HTTP mínimo que imita o Ollama (/api/generate) para testes de
sessiond/modelo_ollama.c e ollama/cliente_ollama.c — NUNCA usado em
produção. Modos controlados por variável de ambiente OLLAMA_FAKE_MODO:

  normal        -- streaming NDJSON normal, chunked, termina com done:true
  truncado      -- fecha a conexão no meio do streaming, sem 'done:true'
  json_quebrado -- envia uma linha que não é JSON válido
  http_erro     -- responde 500
  lento         -- atrasa cada linha (para testar timeout de inatividade)
  ferramenta    -- resposta final inclui um bloco JSON sugerindo uma ferramenta

Uso: OLLAMA_FAKE_MODO=normal python3 ollama_fake.py <porta>
"""
import http.server
import json
import os
import sys
import time

MODO = os.environ.get("OLLAMA_FAKE_MODO", "normal")


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_POST(self):
        tam = int(self.headers.get("Content-Length", 0))
        self.rfile.read(tam)

        if MODO == "http_erro":
            self.send_response(500)
            self.end_headers()
            return

        self.send_response(200)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        linhas = []
        if MODO == "ferramenta":
            linhas = [
                {"response": "Vou olhar o sistema. ", "done": False},
                {"response": '{"ferramenta":"sistema.diagnosticar","parametros":{},'
                             '"justificativa":"verificar a máquina"}', "done": False},
                {"response": "", "done": True},
            ]
        elif MODO == "json_quebrado":
            self._chunk(b"isto nao e json valido\n")
            return
        else:
            linhas = [
                {"response": "ola ", "done": False},
                {"response": "mundo", "done": False},
                {"response": "", "done": True},
            ]

        for i, obj in enumerate(linhas):
            if MODO == "truncado" and i == len(linhas) - 1:
                break  # nunca envia a linha final com done:true
            if MODO == "lento":
                time.sleep(2)
            dados = (json.dumps(obj) + "\n").encode()
            self._chunk(dados)

        if MODO != "truncado":
            self._chunk_fim()
        # em modo truncado: fecha sem o chunk terminador 0\r\n\r\n

    def _chunk(self, dados: bytes):
        self.wfile.write(("%x\r\n" % len(dados)).encode() + dados + b"\r\n")

    def _chunk_fim(self):
        self.wfile.write(b"0\r\n\r\n")


if __name__ == "__main__":
    porta = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    servidor = http.server.HTTPServer(("127.0.0.1", porta), Handler)
    print(servidor.server_port, flush=True)
    servidor.serve_forever()
