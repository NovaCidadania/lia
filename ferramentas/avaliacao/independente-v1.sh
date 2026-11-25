#!/bin/bash
# Avaliação independente v1 (diagnóstico, NÃO cega): 6 tarefas x {sem-modelo, llama3.2:3b, Qwen} x 3 sementes = 54.
# Fail-fast e retomável via matriz.py: só imprime FIM se as 54 tiverem resultado terminal válido (manifesto.json).
# ANTES da matriz: make teste-harness-avaliacao (caso curto de protocolo). Use um diretório de saída NOVO.
# Uso: independente-v1.sh DIR_RESULTADOS   (llama-server do Qwen em 127.0.0.1:8099)
set -u
cd "$(dirname "$0")/../.."
OUT=${1:?diretório de resultados (novo)}
OLL=a80c4f17acd55265feec403c7aef86be0c25983ab279d83f3bcd3abbcb5b8b72
QW=cb8f52cc2bc3f455f36ed8362c6a03beba5c39e61f66db24de37891407423c4f
exec python3 -I ferramentas/avaliacao/matriz.py --tarefas docs/avaliacao/2026-11-25-independente-v1.json --saida "$OUT" \
  --condicao sem-modelo --condicao "ollama:llama3.2:3b=$OLL" --condicao "llamacpp:8099:qwen2.5-coder-7b-q5km=$QW" \
  --sementes 1 2 3 --temperatura 0.4
