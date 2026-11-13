#!/usr/bin/env python3
"""Servidor HTTP mínimo que imita o llama-server nativo (/completion, SSE)
para testes de sessiond/modelo_llama_cpp.c e ollama/cliente_llama_cpp.c —
NUNCA usado em produção. Modos controlados por variável de ambiente
LLAMACPP_FAKE_MODO (espelha testes/integration/ollama_fake.py, adaptado
ao framing SSE em vez de NDJSON):

  normal            -- streaming SSE normal, chunked, termina com stop:true
  truncado          -- fecha a conexão no meio do streaming, sem stop:true
  json_quebrado     -- envia um evento "data:" cujo corpo não é JSON válido
  http_erro         -- responde 500
  lento             -- atrasa cada evento (para testar timeout de inatividade)
  ferramenta        -- resposta final inclui um bloco JSON sugerindo uma ferramenta
  erro_embutido     -- evento com {"error": ...} em vez de "content"/"stop" (HTTP 200)
  chunk_sem_crlf    -- chunk sem o CRLF correto depois dos dados (framing quebrado)
  chunk_extensao    -- tamanho do chunk com uma extensão (";ext=1") não suportada
  cabecalho_gigante -- cabeçalho único acima do limite de 8 KiB do cliente
  prefixo_invalido  -- linha dentro de um evento sem "data:"/":" (formato não reconhecido)

Uso: LLAMACPP_FAKE_MODO=normal python3 llama_cpp_fake.py <porta>
"""
import http.server
import json
import os
import sys
import time

MODO = os.environ.get("LLAMACPP_FAKE_MODO", "normal")


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

        if MODO == "cabecalho_gigante":
            self.send_response(200)
            self.send_header("X-Gigante", "x" * 9000)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self._chunk(b'data: {"content":"","stop":true}\n\n')
            self._chunk_fim()
            return

        self.send_response(200)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        if MODO == "chunk_sem_crlf":
            dados = b'data: {"content":"ola","stop":false}\n\n'
            self.wfile.write(("%x\r\n" % len(dados)).encode() + dados + b"XX")
            return

        if MODO == "chunk_extensao":
            dados = b'data: {"content":"ola","stop":false}\n\n'
            self.wfile.write(("%x;ext=1\r\n" % len(dados)).encode() + dados + b"\r\n")
            self._chunk_fim()
            return

        if MODO == "prefixo_invalido":
            self._chunk(b'evento sem prefixo reconhecido\n\n')
            return

        eventos = []
        if MODO == "ferramenta":
            eventos = [
                {"content": "Vou olhar o sistema. ", "stop": False},
                {"content": '{"ferramenta":"sistema.diagnosticar","parametros":{},'
                             '"justificativa":"verificar a máquina"}', "stop": False},
                {"content": "", "stop": True},
            ]
        elif MODO == "json_quebrado":
            self._chunk(b"data: isto nao e json valido\n\n")
            return
        elif MODO == "erro_embutido":
            self._chunk(b'data: {"error":{"message":"falhou no meio"}}\n\n')
            return
        else:
            eventos = [
                {"content": "ola ", "stop": False},
                {"content": "mundo", "stop": False},
                {"content": "", "stop": True},
            ]

        for i, obj in enumerate(eventos):
            if MODO == "truncado" and i == len(eventos) - 1:
                break  # nunca envia o evento final com stop:true
            if MODO == "lento":
                time.sleep(2)
            dados = ("data: " + json.dumps(obj) + "\n\n").encode()
            self._chunk(dados)

        if MODO != "truncado":
            self._chunk_fim()

    def _chunk(self, dados: bytes):
        self.wfile.write(("%x\r\n" % len(dados)).encode() + dados + b"\r\n")

    def _chunk_fim(self):
        self.wfile.write(b"0\r\n\r\n")


if __name__ == "__main__":
    porta = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    servidor = http.server.HTTPServer(("127.0.0.1", porta), Handler)
    print(servidor.server_port, flush=True)
    servidor.serve_forever()
