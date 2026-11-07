#!/bin/sh
# Benchmark reproduzível de lia_memcpy/lia_memset/lia_memcmp: cada backend
# (escalar, SSE2, AVX2 — os que a CPU suportar) e três tamanhos (64 B,
# 4 KiB, 1 MiB), mediana de 9 repetições (build/lia-mem-bench faz a medição
# e a mediana internamente; este script só roda o binário algumas vezes e
# registra o ambiente). Não é critério de aprovação/reprovação (isso é
# testes/teste.sh via build/lia-mem-teste) — só relatório, comparável ao
# longo do tempo. Ver PLANO.md §7.
#
# Uso: make bench  (chama este script e testes/bench/bench_cat.sh)
set -u
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$RAIZ/build/lia-mem-bench"

if [ ! -x "$BIN" ]; then
    printf 'lia-mem-bench: binário não encontrado em %s\n' "$BIN"
    exit 1
fi

printf 'lia-mem: benchmark por backend/tamanho (mediana de 9 repetições por amostra, dentro do binário)\n'
printf 'ambiente: %s | %s | %s\n\n' \
    "$(uname -srm)" \
    "$(gcc --version | head -1)" \
    "$(awk -F: '/model name/{print $2; exit}' /proc/cpuinfo 2>/dev/null | sed 's/^ *//')"

EXECUCOES=${LIA_BENCH_MEM_REP:-3}
i=0
while [ "$i" -lt "$EXECUCOES" ]; do
    printf -- '--- execução %d/%d ---\n' "$((i + 1))" "$EXECUCOES"
    "$BIN"
    i=$((i + 1))
done

printf '\ntamanho do binário lia-mem-bench: %s bytes\n' "$(stat -c %s "$BIN" 2>/dev/null || echo '?')"
