#!/bin/bash
# Avaliação independente v1: 6 tarefas x {sem-modelo, llama3.2:3b, Qwen} x 3 sementes (T=0.4).
# Conjunto NÃO cego (autoria declarada): diagnóstico, não promoção. Retomável: pula resultado existente.
# Uso: independente-v1.sh DIR_RESULTADOS   (llama-server do Qwen em 127.0.0.1:8099)
set -u
cd "$(dirname "$0")/../.."
OUT=${1:?diretório de resultados}
mkdir -p "$OUT"
A="python3 -I ferramentas/avaliacao/avaliar.py"
TF=docs/avaliacao/2026-11-25-independente-v1.json
OLL=a80c4f17acd55265feec403c7aef86be0c25983ab279d83f3bcd3abbcb5b8b72
QW=cb8f52cc2bc3f455f36ed8362c6a03beba5c39e61f66db24de37891407423c4f
TAREFAS="ind-py-1 ind-py-2 ind-py-3 ind-js-1 ind-js-2 ind-js-3"
roda() { # condicao hash semente tarefa
  local rot="$4__$(echo "$1" | tr ':/' '__')__r$3_T0.4"
  if [ -f "$OUT/$rot.json" ]; then echo "pulado (já existe): $rot"; return; fi
  $A --tarefas $TF --id "$4" --condicao "$1" --rep "$3" --temperatura 0.4 --semente "$3" --modelo-hash "$2" --saida "$OUT" 2>&1
}
for s in 1 2 3; do for t in $TAREFAS; do roda sem-modelo "" $s $t; done; done
for s in 1 2 3; do for t in $TAREFAS; do roda ollama:llama3.2:3b $OLL $s $t; done; done
for s in 1 2 3; do for t in $TAREFAS; do roda llamacpp:8099:qwen2.5-coder-7b-q5km $QW $s $t; done; done
echo FIM
