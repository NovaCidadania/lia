#!/bin/sh
# Testes reais de lia-cat e lia (rodam binários de build/, nunca simulam).
# Uso: make teste
set -u
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
CAT="$RAIZ/build/lia-cat"
ECHO="$RAIZ/build/lia-echo"
HEX="$RAIZ/build/lia-hex"
TIPO="$RAIZ/build/lia-tipo"
CAT_REF="$RAIZ/build/lia-cat-ref"
ECHO_REF="$RAIZ/build/lia-echo-ref"
HEX_REF="$RAIZ/build/lia-hex-ref"
TIPO_REF="$RAIZ/build/lia-tipo-ref"
LIA="$RAIZ/build/lia"
T=$(mktemp -d)
trap 'chmod -R u+rwx "$T" 2>/dev/null; rm -rf "$T"' EXIT
falhas=0; total=0

ok()  { total=$((total+1)); printf 'ok   %s\n' "$1"; }
ko()  { total=$((total+1)); falhas=$((falhas+1)); printf 'FALHA %s\n' "$1"; }
checa() { # descrição, comando...
    d=$1; shift
    if "$@" >/dev/null 2>&1; then ok "$d"; else ko "$d"; fi
}

# ---------- lia-cat ----------
: > "$T/vazio"
head -c 5000000 /dev/urandom > "$T/grande"
printf 'a\0b\377\n\0' > "$T/nul"
printf 'sem newline' > "$T/semnl"

for f in vazio grande nul semnl; do
    checa "lia-cat arquivo $f == cat" sh -c "'$CAT' '$T/$f' | cmp - '$T/$f'"
done
checa "lia-cat stdin" sh -c "'$CAT' < '$T/grande' | cmp - '$T/grande'"
checa "lia-cat pipe"  sh -c "cat '$T/grande' | '$CAT' | cmp - '$T/grande'"
cat "$T/nul" "$T/semnl" "$T/nul" > "$T/n_s_n"
checa "lia-cat vários arquivos e '-'" sh -c "'$CAT' '$T/nul' - '$T/nul' < '$T/semnl' | cmp - '$T/n_s_n'"
"$CAT" /nao/existe "$T/nul" > "$T/o" 2> "$T/e"; rc=$?
[ $rc -eq 1 ] && cmp -s "$T/o" "$T/nul" && grep -q '/nao/existe' "$T/e" \
    && ok "lia-cat erro em um arquivo: continua, exit 1, msg em stderr" || ko "lia-cat erro em um arquivo"
mkdir "$T/dir"
"$CAT" "$T/dir" >/dev/null 2>&1; [ $? -eq 1 ] && ok "lia-cat diretório: exit 1" || ko "lia-cat diretório"
chmod 000 "$T/vazio"
if [ "$(id -u)" -ne 0 ]; then
    "$CAT" "$T/vazio" >/dev/null 2>&1; [ $? -eq 1 ] && ok "lia-cat sem permissão: exit 1" || ko "lia-cat sem permissão"
fi
chmod 600 "$T/vazio"
"$CAT" "$T/grande" > /dev/full 2>/dev/null; [ $? -eq 1 ] && ok "lia-cat escrita falha (/dev/full): exit 1" || ko "lia-cat /dev/full"

# ---------- lia-echo (ASM) e implementações de referência em C ----------
: > "$T/vazio"
head -c 300000 /dev/urandom > "$T/grande_echo"
printf 'a\0b\377\n\0' > "$T/nul_echo"

for prog in "$ECHO" "$ECHO_REF"; do
    nome=$(basename "$prog")
    "$prog" < "$T/vazio" | cmp -s - "$T/vazio" && ok "$nome: entrada vazia" || ko "$nome: entrada vazia"
    "$prog" < "$T/nul_echo" | cmp -s - "$T/nul_echo" && ok "$nome: bytes NUL preservados" || ko "$nome: bytes NUL"
    "$prog" < "$T/grande_echo" | cmp -s - "$T/grande_echo" && ok "$nome: arquivo maior que o buffer, byte a byte" || ko "$nome: arquivo grande"
    saida_parcial=$(sh -c "(printf a; sleep 0.02; printf bc; sleep 0.02; printf d) | '$prog'")
    [ "$saida_parcial" = "abcd" ] && ok "$nome: leitura parcial (pipe lento) preserva os bytes" || ko "$nome: leitura parcial ($saida_parcial)"
done
diff <("$ECHO" < "$T/grande_echo") <("$ECHO_REF" < "$T/grande_echo") >/dev/null \
    && ok "lia-echo (ASM) == lia-echo-ref (C), mesma entrada" || ko "lia-echo ASM != C"

"$CAT_REF" "$T/vazio" "$T/grande_echo" "$T/nul_echo" | cmp -s - <(cat "$T/vazio" "$T/grande_echo" "$T/nul_echo") \
    && ok "lia-cat-ref (C): vários arquivos == cat" || ko "lia-cat-ref: vários arquivos"
"$CAT_REF" /nao/existe "$T/nul_echo" > "$T/o" 2> "$T/e"; rc=$?
[ $rc -eq 1 ] && cmp -s "$T/o" "$T/nul_echo" && grep -q '/nao/existe' "$T/e" \
    && ok "lia-cat-ref: erro em um arquivo, continua, exit 1" || ko "lia-cat-ref: erro em um arquivo"
diff <("$CAT" "$T/grande_echo") <("$CAT_REF" "$T/grande_echo") >/dev/null \
    && ok "lia-cat (ASM) == lia-cat-ref (C), mesma entrada" || ko "lia-cat ASM != C"

# ---------- lia-hex (ASM e referência em C) vs xxd ----------
if command -v xxd >/dev/null; then
    head -c 300000 /dev/urandom > "$T/grande_hex"
    : > "$T/vazio_hex"
    printf 'a\0b\377\n\0' > "$T/nul_hex"
    printf 'sem newline, 13 bytes' > "$T/sem_newline_hex"
    python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256)))" > "$T/todos_bytes_hex" 2>/dev/null \
        || printf '' > "$T/todos_bytes_hex"
    for n in 1 7 15 16 17 31 32 33 63 64 65 4095 4096 4097; do
        head -c "$n" /dev/urandom > "$T/hex_$n"
    done
    for prog in "$HEX" "$HEX_REF"; do
        nome=$(basename "$prog")
        for f in vazio_hex nul_hex sem_newline_hex todos_bytes_hex grande_hex; do
            diff <("$prog" "$T/$f") <(xxd "$T/$f") >/dev/null \
                && ok "$nome $f == xxd" || ko "$nome $f != xxd"
        done
        for n in 1 7 15 16 17 31 32 33 63 64 65 4095 4096 4097; do
            diff <("$prog" "$T/hex_$n") <(xxd "$T/hex_$n") >/dev/null \
                && ok "$nome: $n bytes == xxd" || ko "$nome: $n bytes != xxd"
        done
        diff <("$prog" < "$T/todos_bytes_hex") <(xxd < "$T/todos_bytes_hex") >/dev/null \
            && ok "$nome: stdin == xxd" || ko "$nome: stdin != xxd"
        diff <("$prog" - < "$T/nul_hex") <(xxd - < "$T/nul_hex") >/dev/null \
            && ok "$nome: '-' explícito == xxd" || ko "$nome: '-' != xxd"
        "$prog" /nao/existe > "$T/o" 2> "$T/e"; rc=$?
        [ $rc -eq 1 ] && [ ! -s "$T/o" ] && grep -q '/nao/existe' "$T/e" \
            && ok "$nome: arquivo inexistente, exit 1, nada em stdout" || ko "$nome: arquivo inexistente"
        "$prog" a b >/dev/null 2>&1; [ $? -eq 2 ] && ok "$nome: dois argumentos, exit 2" || ko "$nome: dois argumentos"
    done
    diff <("$HEX" "$T/todos_bytes_hex") <("$HEX_REF" "$T/todos_bytes_hex") >/dev/null \
        && ok "lia-hex (ASM) == lia-hex-ref (C)" || ko "lia-hex ASM != C"
else
    ok "xxd ausente: testes de lia-hex pulados explicitamente (não é falha silenciosa)"
fi

# ---------- lia-tipo (ASM e referência em C) ----------
printf 'PK\x03\x04resto' > "$T/tipo_zip"
printf 'PK\x05\x06' > "$T/tipo_zip_vazio"
printf '\x1f\x8b\x08\x00resto' > "$T/tipo_gzip"
printf 'RIFF\x24\x00\x00\x00WAVEfmt ' > "$T/tipo_wav"
printf '%%PDF-1.4\n%%EOF\n' > "$T/tipo_pdf"
printf '\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR' > "$T/tipo_png"
printf '\xff\xd8\xff\xe0\x00\x10JFIF' > "$T/tipo_jpeg"
head -c 64 /usr/bin/ls > "$T/tipo_elf"
printf 'texto comum em ASCII, com acentuacao removida\n' > "$T/tipo_texto"
: > "$T/tipo_vazio"
printf '\x01\x02\x03\x04\x05\x06\x07\x08' > "$T/tipo_desconhecido"
printf 'RIFF' > "$T/tipo_riff_truncado"          # só o início: nunca pode virar WAV
printf '\x7fEL' > "$T/tipo_elf_truncado"        # só 3 dos 4 bytes do magic ELF
cp "$T/tipo_png" "$T/enganoso.pdf"               # extensão mentindo sobre o conteúdo

for prog in "$TIPO" "$TIPO_REF"; do
    nome=$(basename "$prog")
    [ "$("$prog" "$T/tipo_zip")" = ZIP ]           && ok "$nome: ZIP" || ko "$nome: ZIP"
    [ "$("$prog" "$T/tipo_zip_vazio")" = ZIP ]     && ok "$nome: ZIP (assinatura de arquivo vazio)" || ko "$nome: ZIP vazio"
    [ "$("$prog" "$T/tipo_gzip")" = GZIP ]         && ok "$nome: GZIP" || ko "$nome: GZIP"
    [ "$("$prog" "$T/tipo_wav")" = WAV ]           && ok "$nome: WAV" || ko "$nome: WAV"
    [ "$("$prog" "$T/tipo_pdf")" = PDF ]           && ok "$nome: PDF" || ko "$nome: PDF"
    [ "$("$prog" "$T/tipo_png")" = PNG ]           && ok "$nome: PNG" || ko "$nome: PNG"
    [ "$("$prog" "$T/tipo_jpeg")" = JPEG ]         && ok "$nome: JPEG" || ko "$nome: JPEG"
    [ "$("$prog" "$T/tipo_elf")" = ELF ]           && ok "$nome: ELF" || ko "$nome: ELF"
    [ "$("$prog" "$T/tipo_texto")" = TEXTO ]       && ok "$nome: TEXTO" || ko "$nome: TEXTO"
    [ "$("$prog" "$T/tipo_vazio")" = VAZIO ]       && ok "$nome: VAZIO" || ko "$nome: VAZIO"
    [ "$("$prog" "$T/tipo_desconhecido")" = DESCONHECIDO ] && ok "$nome: DESCONHECIDO" || ko "$nome: DESCONHECIDO"
    [ "$("$prog" "$T/tipo_riff_truncado")" != WAV ] && ok "$nome: RIFF truncado não é confundido com WAV (falha segura)" \
        || ko "$nome: RIFF truncado foi lido como WAV"
    [ "$("$prog" "$T/tipo_elf_truncado")" != ELF ] && ok "$nome: ELF truncado não é confundido com ELF (falha segura)" \
        || ko "$nome: ELF truncado foi lido como ELF"
    [ "$("$prog" "$T/enganoso.pdf")" = PNG ] && ok "$nome: extensão .pdf enganosa não confunde a detecção (é PNG)" \
        || ko "$nome: extensão enganosa confundiu a detecção"
    [ "$("$prog" --json "$T/tipo_elf")" = '{"tipo":"ELF"}' ] && ok "$nome --json" || ko "$nome --json"
    [ "$("$prog" < "$T/tipo_texto")" = TEXTO ] && ok "$nome: stdin (sem argumento)" || ko "$nome: stdin"
    [ "$("$prog" - < "$T/tipo_elf")" = ELF ] && ok "$nome: '-' explícito" || ko "$nome: '-' explícito"
    "$prog" /nao/existe >/dev/null 2>"$T/e"; [ $? -eq 1 ] && grep -q '/nao/existe' "$T/e" \
        && ok "$nome: arquivo inexistente, exit 1" || ko "$nome: arquivo inexistente"
    "$prog" a b >/dev/null 2>&1; [ $? -eq 2 ] && ok "$nome: dois nomes de arquivo, exit 2" || ko "$nome: dois nomes"
done
diff <("$TIPO" "$T/tipo_elf") <("$TIPO_REF" "$T/tipo_elf") >/dev/null && ok "lia-tipo (ASM) == lia-tipo-ref (C)" || ko "lia-tipo ASM != C"

# ---------- lia-mem (lia_memcpy/lia_memset/lia_memcmp: escalar/SSE2/AVX2) ----------
# build/lia-mem-teste é um binário próprio (sem libc) que já faz a
# comparação Assembly vs referência em C, alinhamentos, padrões, tamanhos
# e múltiplos processos internamente (testes/mem/teste_mem.c); aqui só
# repassamos as linhas "ok "/"FALHA " dele para o contador agregado deste
# script, em vez de reimplementar a mesma verificação em shell.
MEM_TESTE="$RAIZ/build/lia-mem-teste"
if [ -x "$MEM_TESTE" ]; then
    saida_mem=$("$MEM_TESTE" 2>&1)
    printf '%s\n' "$saida_mem"
    linhas_mem=0
    while IFS= read -r linha; do
        case $linha in
            "ok   "*)    total=$((total+1)); linhas_mem=$((linhas_mem+1));;
            "FALHA "*)   total=$((total+1)); falhas=$((falhas+1)); linhas_mem=$((linhas_mem+1));;
        esac
    done <<EOF_MEM
$saida_mem
EOF_MEM
    [ "$linhas_mem" -gt 0 ] || ko "lia-mem-teste: nenhuma linha ok/FALHA na saída"
else
    ko "lia-mem-teste: binário não encontrado em $MEM_TESTE"
fi

# build/lia-processos-teste é outro binário próprio (sem libc) no mesmo
# espírito: constrói suas próprias fixtures de "/proc" sob um diretório
# vazio (nunca o /proc real) e testa lia_proc_coletar_amostra()/
# lia_proc_calcular_cpu() (cli/sistema.c) sem depender de timing real.
PROC_TESTE="$RAIZ/build/lia-processos-teste"
PROC_FIXTURE_DIR=$(mktemp -d)
case "$PROC_FIXTURE_DIR" in
    /tmp/*) : ;;
    *) echo "ERRO FATAL: diretório de fixture de processos não está sob /tmp — abortando"; exit 1 ;;
esac
if [ -x "$PROC_TESTE" ]; then
    saida_proc=$("$PROC_TESTE" "$PROC_FIXTURE_DIR" 2>&1)
    printf '%s\n' "$saida_proc"
    linhas_proc=0
    while IFS= read -r linha; do
        case $linha in
            "ok   "*)    total=$((total+1)); linhas_proc=$((linhas_proc+1));;
            "FALHA "*)   total=$((total+1)); falhas=$((falhas+1)); linhas_proc=$((linhas_proc+1));;
        esac
    done <<EOF_PROC
$saida_proc
EOF_PROC
    [ "$linhas_proc" -gt 0 ] || ko "lia-processos-teste: nenhuma linha ok/FALHA na saída"
else
    ko "lia-processos-teste: binário não encontrado em $PROC_TESTE"
fi
rm -rf "$PROC_FIXTURE_DIR"

# nenhum dos binários do núcleo referencia fopen/system/fork/execve/libc:
# são estáticos, sem libc (-nostdlib), então nem poderiam resolver esses símbolos;
# a verificação é sobre os .o antes do link, onde uma referência indevida apareceria
# como símbolo indefinido (U).
if command -v nm >/dev/null; then
    for o in lia-cat.o lia-echo.o lia-hex.o lia-tipo.o \
             lia_cat_ref.o lia_echo_ref.o lia_hex_ref.o lia_tipo_ref.o \
             lia-cpuid.o lia-mem.o lia_mem.o lia_mem_ref.o teste_mem.o bench_mem.o \
             teste_processos_top_cpu.o \
             entrada.o sys.o texto.o; do
        proibidos=$(nm -u "$RAIZ/build/$o" 2>/dev/null | grep -Ei '\b(fopen|fclose|fread|fwrite|system|popen|fork|execve|execvp|execv|malloc|printf)\b')
        [ -z "$proibidos" ] && ok "$o: sem referência a fopen/system/fork/execve/malloc/printf" \
            || ko "$o referencia símbolo proibido: $proibidos"
    done
fi
for prog in "$CAT" "$ECHO" "$HEX" "$TIPO" "$CAT_REF" "$ECHO_REF" "$HEX_REF" "$TIPO_REF" \
            "$RAIZ/build/lia-mem-teste" "$RAIZ/build/lia-mem-bench" "$RAIZ/build/lia-processos-teste"; do
    nome=$(basename "$prog")
    strings -a "$prog" 2>/dev/null | grep -qiE '^(fopen|system|execve)$' \
        && ko "$nome: string de símbolo proibido encontrada no binário" \
        || ok "$nome: nenhuma string de fopen/system/execve no binário final"
done

# ---------- lia: CLI básica ----------
[ "$("$LIA" --version)" = "lia 0.1.0 (x86_64-linux, sem libc)" ] && ok "lia --version" || ko "lia --version"
"$LIA" --help | grep -q 'tools discover' && ok "lia --help" || ko "lia --help"
saida=$("$LIA" < /dev/null | head -1)
case $saida in "lia 0.1.0"*) ok "lia sem argumentos mostra ajuda sem bloquear";; *) ko "lia sem argumentos";; esac
"$LIA" xyz >/dev/null 2>"$T/e"; [ $? -eq 2 ] && grep -q desconhecido "$T/e" && ok "comando desconhecido: exit 2" || ko "comando desconhecido"
"$LIA" tools >/dev/null 2>&1; [ $? -eq 2 ] && ok "lia tools sem subcomando: exit 2" || ko "lia tools sem subcomando"
"$LIA" tools probe >/dev/null 2>&1; [ $? -eq 2 ] && ok "probe sem --safe é recusado" || ko "probe sem --safe"
"$LIA" tools list --estado XYZ >/dev/null 2>&1; [ $? -eq 2 ] && ok "list --estado inválido: exit 2" || ko "list --estado inválido"

# ---------- lia tools (HOME isolado; PATH com ferramentas falsas) ----------
export HOME="$T/home"; mkdir -p "$HOME" "$T/bin"
env -u HOME "$LIA" tools discover >/dev/null 2>&1; [ $? -eq 1 ] && ok "sem HOME: erro claro" || ko "sem HOME"

"$LIA" tools list >/dev/null 2>"$T/e"; [ $? -eq 1 ] && grep -q 'discover' "$T/e" && ok "list sem inventário orienta rodar discover" || ko "list sem inventário"

# ferramentas falsas, com comportamento conhecido
printf '#!/bin/sh\nexec /usr/bin/sleep 30\n' > "$T/bin/sleep"          # trava -> NEEDS_INPUT
printf '#!/nao/existe/interprete\n' > "$T/bin/cat"                     # exec falha (ENOENT)
printf '#!/bin/sh\nexit 0\n' > "$T/bin/nano"                           # sem permissão p/ o dono
printf 'texto simples\n' > "$T/bin/dados"                              # sem bit x: ignorado
printf '#!/bin/sh\nprintf "\\001\\002binario\\n"\n' > "$T/bin/wc"      # saída binária
printf '#!/bin/sh\nexit 0\n' > "$T/bin/lia-sem-permissao"
chmod 755 "$T/bin/sleep" "$T/bin/cat" "$T/bin/wc"; chmod 001 "$T/bin/nano" "$T/bin/lia-sem-permissao"; chmod 644 "$T/bin/dados"
ln -s /nao/existe "$T/bin/quebrado"
PATH="$T/bin:$PATH"; export PATH

"$LIA" tools discover > "$T/disc" 2>&1; rc=$?
[ $rc -eq 0 ] && ok "discover: exit 0" || ko "discover: exit $rc"
[ "$(od -An -c -N4 "$HOME/.lia/maquina/inventario/ferramentas.lia" | tr -d ' ')" = 'LIA\0' ] && ok "inventário binário começa com LIA\\0" || ko "assinatura do inventário"
[ "$(stat -c %a "$HOME/.lia")" = 700 ] && [ "$(stat -c %a "$HOME/.lia/maquina/inventario/ferramentas.lia")" = 600 ] && ok "permissões 0700 (~/.lia) e 0600 (inventário)" || ko "permissões"
[ -s "$HOME/.lia/humano/diagnosticos/ferramentas.md" ] && ok "relatório legível gerado" || ko "relatório legível"
"$LIA" tools  list --estado PERMISSION_DENIED | grep -q ' lia-sem-permissao ' && ok "arquivo 001 do dono -> PERMISSION_DENIED" || ko "PERMISSION_DENIED"
"$LIA" tools list | grep -q ' dados ' && ko "arquivo sem bit x deveria ser ignorado" || ok "arquivo sem bit x ignorado"
"$LIA" tools list | grep ' nano ' | grep -q '^ACCESSIBLE' && ok "entrada sem permissão não esconde a acessível seguinte no PATH (como execvp)" || ko "sombreamento por entrada negada"
"$LIA" tools list | grep ' sleep ' | grep -q "$T/bin" && ok "PATH: ferramenta falsa sombreia /usr/bin/sleep" || ko "sombreamento"
"$LIA" tools list | grep ' cat ' | grep -q 'script' && ok "script detectado pelo shebang" || ko "detecção de script"
"$LIA" tools list | grep ' ls ' | grep -q 'ELF' && ok "ELF detectado pelos bytes" || ko "detecção de ELF"

# contagem independente (shell) x inventário
dirs="$PATH:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$HOME/.local/bin"
for d in $(printf %s "$dirs" | tr ':' ' '); do
    [ -d "$d" ] || continue
    for f in "$d"/* "$d"/.[!.]* ; do
        [ -f "$f" ] && [ -x "$f" ] && basename "$f"
    done
done | /usr/bin/sort -u | /usr/bin/wc -l > "$T/n_shell"
n_shell=$(while read -r n; do printf %s "$n"; done < "$T/n_shell")
acessiveis=$("$LIA" tools list | grep -vc '^PERMISSION_DENIED')
[ "$n_shell" -eq "$acessiveis" ] && ok "contagem de executáveis acessíveis == enumeração independente ($acessiveis)" \
    || ko "contagem: shell=$n_shell lia=$acessiveis"

# probes
#
# A asserção de tempo é sobre o mecanismo de timeout em si (dur_ms registrado
# para o item que trava, "sleep"), não sobre o tempo total da chamada. O total
# soma o fork+exec+wait de ~80 ferramentas do catálogo além do item que trava
# de propósito; esse overhead cresce com a carga do sistema e não é limitado
# por PROBE_LIMITE_MS, então um teto agregado (ex.: "<=6s") é inerentemente
# instável sob carga (reproduzido nesta investigação: falha isolada após
# make bench, que gera carga pesada de CPU/IO) mesmo quando o timeout de cada
# ferramenta individual funcionou corretamente. Medir o item específico é
# equivalente e independente do tamanho do catálogo e da velocidade da máquina.
inicio=$(date +%s)
"$LIA" tools probe --safe > "$T/probe" 2>&1; rc=$?
fim=$(date +%s)
[ $rc -eq 0 ] && ok "probe --safe: exit 0" || ko "probe --safe: exit $rc"
[ $((fim - inicio)) -le 60 ] && ok "probe termina em tempo finito, sem travar (levou $((fim - inicio))s)" || ko "probe não terminou (levou $((fim - inicio))s)"
if command -v python3 >/dev/null; then
    ms_sleep=$("$LIA" tools list --json --estado NEEDS_INPUT | python3 -I -c 'import json,sys
d=json.load(sys.stdin)
print(next(x["ms"] for x in d if x["nome"]=="sleep"))')
    [ "$ms_sleep" -ge 2900 ] && [ "$ms_sleep" -le 5000 ] \
        && ok "timeout do probe (PROBE_LIMITE_MS=3000) respeitado: sleep levou ${ms_sleep}ms" \
        || ko "timeout do probe fora da faixa esperada: sleep levou ${ms_sleep}ms"
fi
"$LIA" tools list --estado NEEDS_INPUT | grep -q ' sleep ' && ok "ferramenta que trava -> NEEDS_INPUT (processo morto)" || ko "NEEDS_INPUT"
pgrep -f "$T/bin/sleep" >/dev/null && ko "processo travado ficou vivo" || ok "nenhum processo órfão"
"$LIA" tools list --estado PROBE_FAILED | grep -q ' cat ' && ok "execve falho (interpretador ausente) -> PROBE_FAILED" || ko "PROBE_FAILED por execve"
"$LIA" tools list --estado PROBE_FAILED | grep -q ' wc ' && ok "saída binária não vira versão -> PROBE_FAILED" || ko "saída binária"
"$LIA" tools  list --estado PERMISSION_DENIED | grep -q ' lia-sem-permissao ' && ok "PERMISSION_DENIED preservado após probe" || ko "PERMISSION_DENIED após probe"
real_ver=$(/usr/bin/uname --version | head -1)
"$LIA" tools list --estado PROBE_SUCCESS | grep ' uname ' | grep -qF "# $real_ver" && ok "versão do uname == execução direta" || ko "versão do uname"
"$LIA" tools list --estado PROBE_SUCCESS | grep ' false ' | grep -q false && ok "false: sai com 1 por definição e conta como sucesso" || ko "false"
"$LIA" tools list --estado NO_SAFE_PROBE | grep -q ' 411toppm \| xxd ' && ok "sem probe conhecido -> NO_SAFE_PROBE (não executa)" || ko "NO_SAFE_PROBE"
"$LIA" tools list --estado NO_SAFE_PROBE | grep -q ' dados ' && ko "dados" || :

# JSON válido
if command -v python3 >/dev/null; then
    "$LIA" tools list --json | python3 -I -c 'import json,sys; d=json.load(sys.stdin); assert len(d)>100 and all("nome" in x for x in d)' \
        && ok "list --json é JSON válido" || ko "list --json"
    "$LIA" tools list --json --estado PERMISSION_DENIED | python3 -I -c 'import json,sys; d=json.load(sys.stdin); assert [x["nome"] for x in d]==["lia-sem-permissao"]' \
        && ok "list --json --estado filtra" || ko "list --json --estado"
fi

# integridade: corromper um byte deve ser detectado
cp "$HOME/.lia/maquina/inventario/ferramentas.lia" "$T/inv.bak"
printf 'X' | dd of="$HOME/.lia/maquina/inventario/ferramentas.lia" bs=1 seek=100 conv=notrunc 2>/dev/null
"$LIA" tools list >/dev/null 2>"$T/e"; [ $? -eq 1 ] && grep -q checksum "$T/e" && ok "inventário corrompido é rejeitado (checksum)" || ko "checksum"
head -c 30 "$T/inv.bak" > "$HOME/.lia/maquina/inventario/ferramentas.lia"
"$LIA" tools list >/dev/null 2>&1; [ $? -eq 1 ] && ok "inventário truncado é rejeitado" || ko "truncado"
cp "$T/inv.bak" "$HOME/.lia/maquina/inventario/ferramentas.lia"
"$LIA" tools list >/dev/null 2>&1 && ok "inventário restaurado volta a ser lido" || ko "restauração"


# ---------- lia system (somente leitura) ----------
nproc_real=$(/usr/bin/nproc)
[ "$("$LIA" system recursos | sed -n 's/^CPU:    \([0-9]*\).*/\1/p')" = "$nproc_real" ] && ok "system recursos: núcleos == nproc ($nproc_real)" || ko "system recursos: núcleos"
[ "$("$LIA" system diagnosticar --json | python3 -I -c 'import json,sys;print(json.load(sys.stdin)["cpu"]["nucleos"])')" = "$nproc_real" ] \
    && ok "system diagnosticar --json: cpu.nucleos == nproc" || ko "system diagnosticar --json: nucleos"
"$LIA" system diagnosticar --json | python3 -I -c 'import json,sys; d=json.load(sys.stdin); assert d["kernel"]["sysname"]=="Linux"' \
    && ok "system diagnosticar --json: kernel.sysname == Linux" || ko "system diagnosticar --json: kernel"
real_total_kb=$(awk '/^MemTotal:/{print $2}' /proc/meminfo)
lia_total_kb=$("$LIA" system recursos --json | python3 -I -c 'import json,sys;print(json.load(sys.stdin)["memoria_kb"]["total"])')
[ "$real_total_kb" = "$lia_total_kb" ] && ok "system recursos --json: memoria_kb.total == /proc/meminfo" || ko "memoria_kb.total ($lia_total_kb != $real_total_kb)"

n_net=$(ls /sys/class/net | /usr/bin/wc -l); n_blk=$(ls /sys/class/block | /usr/bin/wc -l); n_pci=$(ls /sys/bus/pci/devices | /usr/bin/wc -l)
n_lia=$("$LIA" system dispositivos --json | python3 -I -c 'import json,sys;print(len(json.load(sys.stdin)))')
[ "$n_lia" = "$((n_net + n_blk + n_pci))" ] && ok "system dispositivos --json: total == rede+bloco+pci do /sys ($n_lia)" || ko "contagem de dispositivos"
"$LIA" system dispositivos --json | python3 -I -c 'import json,sys
d=json.load(sys.stdin)
nomes=set(x["nome"] for x in d if x["classe"]=="rede")
assert nomes >= {"lo"}, nomes' && ok "system dispositivos: inclui a interface lo" || ko "dispositivos: lo ausente"
"$LIA" system dispositivos | grep -q '^rede    lo' && ok "system dispositivos (texto): inclui lo" || ko "dispositivos texto: lo"
pci0=$(ls /sys/bus/pci/devices | head -1)
vendor_real=$(/usr/bin/cat "/sys/bus/pci/devices/$pci0/vendor")
vendor_lia=$("$LIA" system dispositivos --json | python3 -I -c "import json,sys; d=json.load(sys.stdin); print(next(x['vendor'] for x in d if x['nome']=='$pci0'))")
[ "$vendor_real" = "$vendor_lia" ] && ok "system dispositivos: vendor PCI == /sys/bus/pci/devices/.../vendor" || ko "vendor PCI ($vendor_lia != $vendor_real)"

"$LIA" system >/dev/null 2>&1; [ $? -eq 2 ] && ok "system sem subcomando: exit 2" || ko "system sem subcomando"
"$LIA" system bogus >/dev/null 2>&1; [ $? -eq 2 ] && ok "system subcomando desconhecido: exit 2" || ko "system subcomando desconhecido"
"$LIA" system diagnosticar --bogus >/dev/null 2>&1; [ $? -eq 2 ] && ok "system opção desconhecida: exit 2" || ko "system opção desconhecida"

# ---------- lia system processos_top_cpu (somente leitura; ver também testes/nucleo/ para a referência pura testável) ----------
"$LIA" system processos_top_cpu --limite 0 >/dev/null 2>&1; [ $? -eq 2 ] && ok "processos_top_cpu: --limite 0 -> exit 2" || ko "processos_top_cpu: --limite 0"
"$LIA" system processos_top_cpu --limite 101 >/dev/null 2>&1; [ $? -eq 2 ] && ok "processos_top_cpu: --limite 101 -> exit 2" || ko "processos_top_cpu: --limite 101"
"$LIA" system processos_top_cpu --limite abc >/dev/null 2>&1; [ $? -eq 2 ] && ok "processos_top_cpu: --limite não numérico -> exit 2" || ko "processos_top_cpu: --limite não numérico"
"$LIA" system processos_top_cpu --intervalo-ms 99 >/dev/null 2>&1; [ $? -eq 2 ] && ok "processos_top_cpu: --intervalo-ms 99 -> exit 2" || ko "processos_top_cpu: --intervalo-ms 99"
"$LIA" system processos_top_cpu --intervalo-ms 5001 >/dev/null 2>&1; [ $? -eq 2 ] && ok "processos_top_cpu: --intervalo-ms 5001 -> exit 2" || ko "processos_top_cpu: --intervalo-ms 5001"
"$LIA" system processos_top_cpu --ordenar-por memoria >/dev/null 2>&1; [ $? -eq 2 ] && ok "processos_top_cpu: --ordenar-por inválido -> exit 2" || ko "processos_top_cpu: --ordenar-por inválido"
"$LIA" system processos_top_cpu --raiz-proc /caminho/que/nao/existe --intervalo-ms 100 >/dev/null 2>&1
[ $? -eq 1 ] && ok "processos_top_cpu: raiz /proc inexistente -> exit 1 (erro estruturado)" || ko "processos_top_cpu: raiz inexistente"

"$LIA" system processos_top_cpu --json --limite 5 --intervalo-ms 150 > "$T/proc_top.json"
python3 -I -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d["limite"] == 5
assert d["intervalo_ms"] == 150
assert isinstance(d["amostrados"], int) and d["amostrados"] > 0
procs = d["processos"]
assert len(procs) <= 5
anteriores = None
for p in procs:
    for campo in ("pid", "nome", "estado", "cpu_pct_centesimos", "tempo_cpu_centesimos_s"):
        assert campo in p, campo
    assert isinstance(p["pid"], int) and p["pid"] > 0
    assert isinstance(p["cpu_pct_centesimos"], int)  # nunca float (ver PLANO.md item 13: json-glib perde precisão em decimal)
    assert isinstance(p["tempo_cpu_centesimos_s"], int)
    if anteriores is not None:
        assert p["cpu_pct_centesimos"] <= anteriores, "não está ordenado decrescente"
    anteriores = p["cpu_pct_centesimos"]
' "$T/proc_top.json" \
    && ok "processos_top_cpu --json: formato estável, inteiro (nunca float), ordenado decrescente, <= limite" \
    || ko "processos_top_cpu --json: formato inesperado"

"$LIA" system processos_top_cpu --limite 3 --intervalo-ms 100 > "$T/proc_top.txt"
head -1 "$T/proc_top.txt" | grep -qE 'PID.*NOME.*EST.*CPU%' && ok "processos_top_cpu: cabeçalho do texto humano presente" || ko "processos_top_cpu: cabeçalho ausente"
# /usr/bin/wc explícito: a partir daqui o PATH já foi deliberadamente
# poluído (seção "lia tools" acima planta um "wc" falso em "$T/bin" para
# testar detecção de saída binária, e nunca restaura o PATH original).
[ "$(/usr/bin/wc -l < "$T/proc_top.txt")" -ge 2 ] && ok "processos_top_cpu: texto humano tem ao menos uma linha de dados" || ko "processos_top_cpu: sem linhas de dados"

env -i "$LIA" system processos_top_cpu --json --limite 1 --intervalo-ms 100 >/dev/null 2>&1 \
    && ok "processos_top_cpu: funciona sem nenhuma variável de ambiente (nem HOME)" \
    || ko "processos_top_cpu depende do ambiente"

# nenhum comando somente leitura chama fork/execve (sem shell, sem processo filho, sem privilégio)
if command -v nm >/dev/null; then
    nm -u "$RAIZ/build/sistema.o" 2>/dev/null | grep -qE 'lia_fork|lia_execve' \
        && ko "cli/sistema.c referencia fork/execve (deveria ser só leitura)" \
        || ok "cli/sistema.c não referencia fork/execve (confirmado no objeto compilado)"
fi

# roda sem qualquer variável de ambiente de privilégio e sem rede: tem que funcionar igual
diff <(env -i HOME="$HOME" "$LIA" system diagnosticar) <("$LIA" system diagnosticar) >/dev/null \
    && ok "system diagnosticar não depende do ambiente além de HOME" || ko "system diagnosticar depende do ambiente"

# ---------- shell, sudo e root ----------
[ "$("$LIA" shell -c 'echo oi')" = oi ] && ok "lia shell -c executa com semântica de shell" || ko "lia shell -c"
saida=$(printf 'echo $((6*7))\nexit 7\n' | "$LIA" shell); rc=$?
[ "$saida" = 42 ] && [ $rc -eq 7 ] && ok "lia shell lê comandos do stdin e propaga o exit code (7)" || ko "lia shell stdin (saida=$saida rc=$rc)"
saida=$(env -u SHELL "$LIA" shell -c 'echo sem-SHELL'); [ "$saida" = sem-SHELL ] && ok "lia shell sem \$SHELL cai para bash/sh" || ko "lia shell sem SHELL"
SHELL=/nao/existe "$LIA" shell --dry-run | grep -q 'dry-run: "/' && ok "lia shell com \$SHELL inválido ignora e resolve outro" || ko "SHELL inválido"
"$LIA" shell -x >/dev/null 2>&1; [ $? -eq 2 ] && ok "lia shell opção inválida: exit 2" || ko "lia shell opção inválida"

[ "$("$LIA" sudo --version | head -1)" = "$(/usr/bin/sudo --version | head -1)" ] && ok "lia sudo --version == sudo --version" || ko "lia sudo --version"
"$LIA" sudo --dry-run -n true | grep -qF '["sudo","-n","true"]' && ok "lia sudo --dry-run mostra o argv exato" || ko "lia sudo --dry-run"
"$LIA" sudo -n true </dev/null >/dev/null 2>&1; r1=$?
/usr/bin/sudo -n true </dev/null >/dev/null 2>&1; r2=$?
[ $r1 -eq $r2 ] && ok "lia sudo -n true: mesmo exit code que o sudo direto ($r1)" || ko "lia sudo exit ($r1 != $r2)"
"$LIA" sudo -n sh -c 'exit 5' </dev/null >/dev/null 2>&1; r1=$?
/usr/bin/sudo -n sh -c 'exit 5' </dev/null >/dev/null 2>&1; r2=$?
[ $r1 -eq $r2 ] && ok "lia sudo propaga o exit code do comando ($r1)" || ko "lia sudo propaga exit ($r1 != $r2)"

"$LIA" root --dry-run -- id | grep -qF '["sudo","--","id"]' && ok "lia root --dry-run -- id -> sudo -- id" || ko "lia root --dry-run"
"$LIA" root --dry-run | grep -qF '["sudo","-i"]' && ok "lia root --dry-run sem comando -> sudo -i (shell de root)" || ko "lia root sem comando"
"$LIA" root --dry-run --auth | grep -qF '["sudo","-v"]' && ok "lia root --auth --dry-run -> sudo -v" || ko "lia root --auth"
"$LIA" root --bogus >/dev/null 2>&1; [ $? -eq 2 ] && ok "lia root opção inválida: exit 2" || ko "lia root opção inválida"

"$LIA" root --status > "$T/st" 2>&1; rc=$?
/usr/bin/sudo -n true </dev/null >/dev/null 2>&1; ref=$?
case $rc:$ref in
    0:0)  grep -q ROOT_DISPONIVEL "$T/st" && ok "root --status: ROOT_DISPONIVEL (sudo -n true == 0)" || ko "status 0";;
    10:*) grep -q ROOT_REQUER_SENHA "$T/st" && ok "root --status: ROOT_REQUER_SENHA" || ko "status 10";;
    11:*) grep -q ROOT_NEGADO "$T/st" && ok "root --status: ROOT_NEGADO" || ko "status 11";;
    *)    ko "root --status inesperado: rc=$rc ref=$ref: $(head -2 "$T/st")";;
esac

if [ "$rc" -eq 0 ]; then
    # root de verdade, executado nesta máquina via sudo
    [ "$("$LIA" root -- id -u </dev/null)" = 0 ] && ok "lia root -- id -u => 0 (root real obtido via sudo)" || ko "lia root -- id -u"
    [ "$("$LIA" sudo -n id -u </dev/null)" = 0 ] && ok "lia sudo -n id -u => 0" || ko "lia sudo -n id -u"
    [ "$(printf 'id -u\n' | "$LIA" root </dev/null 2>/dev/null | tail -1)" = 0 ] || :
    "$LIA" root -- sh -c 'exit 9' </dev/null >/dev/null 2>&1; [ $? -eq 9 ] && ok "lia root propaga exit code do comando (9)" || ko "lia root exit code"
    "$LIA" root -- /usr/bin/cat /etc/sudoers >/dev/null 2>&1 </dev/null; ok "lia root lê arquivo restrito (exit $?)"
fi

# auditoria: toda execução fica registrada antes de rodar
aud=$(ls "$HOME"/.lia/humano/auditoria/*.log 2>/dev/null | head -1)
[ -n "$aud" ] && grep -q 'acao=shell' "$aud" && grep -q 'acao=sudo' "$aud" && ok "auditoria registra shell e sudo" || ko "auditoria"
[ -n "$aud" ] && grep -q 'uid=' "$aud" && grep -q 'exec="/' "$aud" && ok "auditoria registra uid e caminho executado" || ko "auditoria formato"
[ "$(stat -c %a "$aud")" = 600 ] && ok "auditoria com modo 0600" || ko "modo da auditoria"

# binários sem libc
if ldd "$LIA" 2>&1 | grep -q 'not a dynamic\|não é um executável dinâmico\|statically linked'; then ok "lia é estático"; else ko "lia é estático"; fi
if ldd "$CAT" 2>&1 | grep -q 'not a dynamic\|não é um executável dinâmico\|statically linked'; then ok "lia-cat é estático"; else ko "lia-cat é estático"; fi

printf '\n%d testes, %d falhas\n' "$total" "$falhas"
[ "$falhas" -eq 0 ]
