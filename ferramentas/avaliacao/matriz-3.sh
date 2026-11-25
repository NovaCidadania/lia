#!/bin/bash
# Matriz 3 (diagnóstico do efeito das correções 5d7ebdc): 4 tarefas do holdout JÁ VISTO x 2 modelos x 3 sementes.
# NÃO serve para promoção (holdout deixou de ser cego). Retomável: pula a execução cujo resultado já existe.
# Uso: matriz-3.sh DIR_RESULTADOS   (llama-server do Qwen precisa estar em 127.0.0.1:8099)
set -u
cd "$(dirname "$0")/../.."
OUT=${1:?diretório de resultados}
mkdir -p "$OUT"
A="python3 -I ferramentas/avaliacao/avaliar.py"
DH=docs/avaliacao/2026-11-24-dev-holdout.json
OLL=a80c4f17acd55265feec403c7aef86be0c25983ab279d83f3bcd3abbcb5b8b72
QW=cb8f52cc2bc3f455f36ed8362c6a03beba5c39e61f66db24de37891407423c4f
HOLD="ho-sf-1 ho-sf-2 ho-le-1 ho-le-2"
roda() { # condicao hash semente tarefa
  local rot="$4__$(echo "$1" | tr ':/' '__')__r$3_T0.4"
  if [ -f "$OUT/$rot.json" ]; then echo "pulado (já existe): $rot"; return; fi
  $A --tarefas $DH --id "$4" --condicao "$1" --rep "$3" --temperatura 0.4 --semente "$3" --modelo-hash "$2" --saida "$OUT" 2>&1
}
for s in 1 2 3; do for t in $HOLD; do roda ollama:llama3.2:3b $OLL $s $t; done; done
for s in 1 2 3; do for t in $HOLD; do roda llamacpp:8099:qwen2.5-coder-7b-q5km $QW $s $t; done; done
echo FIM
