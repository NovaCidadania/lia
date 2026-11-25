#!/usr/bin/env python3
"""Servidor HTTP mínimo que imita o Ollama (/api/generate) para testes de
sessiond/modelo_ollama.c e ollama/cliente_ollama.c — NUNCA usado em
produção. Modos controlados por variável de ambiente OLLAMA_FAKE_MODO:

  normal           -- streaming NDJSON normal, chunked, termina com done:true
  truncado         -- fecha a conexão no meio do streaming, sem 'done:true'
  limite           -- streaming normal; o objeto final diz done_reason "length" (num_predict)
  json_quebrado    -- envia uma linha que não é JSON válido
  http_erro        -- responde 500
  lento            -- atrasa cada linha (para testar timeout de inatividade)
  ferramenta       -- resposta final inclui um bloco JSON sugerindo uma ferramenta
  estruturado      -- resposta é, inteira, o objeto JSON do formato estruturado; só
                      responde assim se a requisição trouxe "format" (esquema) com
                      a ferramenta no enum — senão devolve texto dizendo o que faltou
  chunk_sem_crlf   -- chunk sem o CRLF correto depois dos dados (framing quebrado)
  chunk_extensao   -- tamanho do chunk com uma extensão (";ext=1") não suportada
  cabecalho_gigante -- cabeçalho único acima do limite de 8 KiB do cliente

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
        corpo = self.rfile.read(tam)
        try:
            pedido = json.loads(corpo)
        except ValueError:
            pedido = {}

        if MODO == "http_erro":
            self.send_response(500)
            self.end_headers()
            return

        if MODO == "cabecalho_gigante":
            self.send_response(200)
            # um único cabeçalho acima do limite de 8 KiB do cliente —
            # nunca deveria ser esperado indefinidamente nem aceito.
            self.send_header("X-Gigante", "x" * 9000)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self._chunk(b'{"response":"ok","done":true}\n')
            self._chunk_fim()
            return

        self.send_response(200)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        if MODO == "chunk_sem_crlf":
            dados = b'{"response":"ola","done":false}\n'
            self.wfile.write(("%x\r\n" % len(dados)).encode() + dados + b"XX")  # nunca \r\n de verdade
            return

        if MODO == "chunk_extensao":
            dados = b'{"response":"ola","done":false}\n'
            # extensão de chunk (RFC 7230 §4.1.1) — o cliente não suporta
            # nenhuma e deve rejeitar, não ignorar silenciosamente.
            self.wfile.write(("%x;ext=1\r\n" % len(dados)).encode() + dados + b"\r\n")
            self._chunk_fim()
            return

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

        if MODO == "estruturado":
            fmt = pedido.get("format")
            nomes = []
            if isinstance(fmt, dict):
                nomes = fmt.get("properties", {}).get("ferramenta", {}).get("enum", [])
            prompt = pedido.get("prompt", "")
            if "DADOS REAIS DEVOLVIDOS" in prompt:
                # pedido de resumo: texto livre, e SEM "format" (senão o fake denuncia)
                texto = "resumo sem format ok" if fmt is None else "ERRO: resumo veio com format"
            elif "mock.leitura" in nomes and pedido.get("options", {}).get("num_predict", 0) > 0:
                texto = json.dumps({"tipo": "proposta", "ferramenta": "mock.leitura", "parametros": {},
                                    "justificativa": "teste", "resposta": "vou ler"})
            else:
                texto = "ERRO: requisicao sem format/enum de ferramentas/num_predict"
            meio = len(texto) // 2
            linhas = [{"response": texto[:meio], "done": False}, {"response": texto[meio:], "done": False},
                      {"response": "", "done": True}]

        if MODO == "ecoar_opcoes":   # devolve as opções recebidas (num_ctx, temperatura, semente)
            txt = json.dumps(pedido.get("options", {}), sort_keys=True)
            linhas = [{"response": txt, "done": False}, {"response": "", "done": True, "done_reason": "stop"}]
        if MODO == "limite":
            linhas[-1] = {"response": "", "done": True, "done_reason": "length", "eval_count": 7, "prompt_eval_count": 3}
        elif MODO == "normal":
            linhas[-1] = dict(linhas[-1], done_reason="stop", eval_count=2)
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
