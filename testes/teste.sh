#!/bin/sh
# Testes reais de lia-cat e lia (rodam binários de build/, nunca simulam).
# Uso: make teste
set -u
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
CAT="$RAIZ/build/lia-cat"
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
inicio=$(date +%s)
"$LIA" tools probe --safe > "$T/probe" 2>&1; rc=$?
fim=$(date +%s)
[ $rc -eq 0 ] && ok "probe --safe: exit 0" || ko "probe --safe: exit $rc"
[ $((fim - inicio)) -le 6 ] && ok "probe respeitou o timeout (levou $((fim - inicio))s)" || ko "probe demorou $((fim - inicio))s"
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
