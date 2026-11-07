#!/bin/sh
# Benchmark reproduzível: lia-cat (Assembly) vs lia-cat-ref (C) vs cat (coreutils).
#
# Não é um teste de corretude (isso é testes/teste.sh) e não exige que a Lia
# seja mais rápida nesta primeira implementação (PLANO.md, fase 1): apenas
# mede e registra vazão, latência e tamanho do binário, para comparação ao
# longo do tempo (PLANO.md §7 — "cada PR que toca o núcleo reporta antes/depois").
#
# Uso: make bench
set -u
RAIZ=$(cd "$(dirname "$0")/../.." && pwd)
B="$RAIZ/build"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

REPETICOES=${LIA_BENCH_REP:-7}
TAM_MIB=${LIA_BENCH_MIB:-256}

dd if=/dev/urandom of="$T/entrada" bs=1M count="$TAM_MIB" status=none

tempo_ms() {
    # tempo_ms <comando...> — melhor de $REPETICOES execuções, em milissegundos
    melhor=""
    i=0
    while [ "$i" -lt "$REPETICOES" ]; do
        ini=$(date +%s%N)
        "$@" < "$T/entrada" > "$T/saida" 2>/dev/null
        fim=$(date +%s%N)
        ms=$(( (fim - ini) / 1000000 ))
        if [ -z "$melhor" ] || [ "$ms" -lt "$melhor" ]; then melhor=$ms; fi
        i=$((i + 1))
    done
    printf '%s' "$melhor"
}

linha() {
    nome=$1; bin=$2
    if [ ! -x "$bin" ]; then
        printf '%-16s indisponível (%s)\n' "$nome" "$bin"
        return
    fi
    ms=$(tempo_ms "$bin")
    cmp -s "$T/entrada" "$T/saida" && corretude=ok || corretude=DIVERGIU
    tam=$(stat -c %s "$bin" 2>/dev/null || echo "?")
    if [ "$ms" -gt 0 ]; then
        mibs=$(( TAM_MIB * 1000 / ms ))
    else
        mibs="inf"
    fi
    printf '%-16s %8s ms   %8s MiB/s   %8s bytes   %s\n' "$nome" "$ms" "$mibs" "$tam" "$corretude"
}

printf 'lia-cat: benchmark de %s MiB, melhor de %s execuções (não é critério de aceite nesta fase)\n\n' "$TAM_MIB" "$REPETICOES"
printf '%-16s %11s   %11s   %14s   %s\n' "programa" "tempo" "vazão" "tamanho" "saída"
linha "cat (coreutils)" "$(command -v cat)"
linha "lia-cat (ASM)"   "$B/lia-cat"
linha "lia-cat-ref (C)" "$B/lia-cat-ref"
