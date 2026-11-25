#!/bin/bash
# Cria os repositórios-base dos fixtures da avaliação independente a partir de docs/avaliacao/fixtures/.
# Determinístico (autor/data fixos): o mesmo conteúdo gera o mesmo commit-base. Não sobrescreve repositório existente.
# Uso: criar-fixtures.sh [DESTINO]   (padrão: ~/.lia/avaliacao-fixtures)
set -eu
cd "$(dirname "$0")/../.."
DEST=${1:-$HOME/.lia/avaliacao-fixtures}
mkdir -p "$DEST"
for d in docs/avaliacao/fixtures/*/; do
  n=$(basename "$d")
  if [ -e "$DEST/$n/.git" ]; then echo "$n: já existe ($(git -C "$DEST/$n" rev-parse --short=12 HEAD))"; continue; fi
  mkdir -p "$DEST/$n"
  cp -r "$d". "$DEST/$n/"
  find "$DEST/$n" -name __pycache__ -type d -prune -exec rm -r {} + 2>/dev/null || true
  git -C "$DEST/$n" init -q -b master
  git -C "$DEST/$n" add -A
  GIT_AUTHOR_NAME=lia-avaliacao GIT_AUTHOR_EMAIL=avaliacao@lia.invalid GIT_COMMITTER_NAME=lia-avaliacao GIT_COMMITTER_EMAIL=avaliacao@lia.invalid \
  GIT_AUTHOR_DATE="2026-11-25T00:00:00+0000" GIT_COMMITTER_DATE="2026-11-25T00:00:00+0000" \
    git -C "$DEST/$n" commit -q -m "fixture $n (base da avaliação independente v1)"
  echo "$n: criado ($(git -C "$DEST/$n" rev-parse --short=12 HEAD))"
done
