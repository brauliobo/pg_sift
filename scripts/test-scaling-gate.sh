#!/usr/bin/env bash
# scripts/scaling-gate.sh build, run by two jobs at once (scaling_slope and sb_mask_small of one CI run): both get the root, and the library is built once. make is a stub
# that takes two seconds; the sources are those of this tree. Seconds, no server (it needs the pg_config of PostgreSQL 18 or later, which the script itself checks).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
bad=0
fail() { echo "FAIL: $*"; bad=1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir "$T/bin"
cat >"$T/bin/make" <<'STUB'
#!/usr/bin/env bash
echo run >>"$STUB_RUNS"
sleep 2
for a in "$@"; do case $a in DESTDIR=*) mkdir -p "${a#DESTDIR=}"; esac; done
STUB
chmod +x "$T/bin/make"
run() { PATH=$T/bin:$PATH STUB_RUNS=$T/runs SIFT_SB_DIR=$T/sb/key "$REPO/scripts/scaling-gate.sh" build 2>"$T/err.$1"; echo "$? $1" >>"$T/rc"; }
run a >"$T/out.a" & pa=$!
run b >"$T/out.b" & pb=$!
wait "$pa" "$pb"
if grep -qx '77 [ab]' "$T/rc"; then echo "scaling-gate: skipped (needs PostgreSQL 18)"; exit 0; fi
[[ $(sort "$T/rc" | tr '\n' ' ') == "0 a 0 b " ]] || fail "a concurrent build failed: $(cat "$T/rc" | tr '\n' ' ') $(cat "$T/err.a" "$T/err.b")"
[[ $(cat "$T/out.a") == "$T/sb/key/root" && $(cat "$T/out.b") == "$T/sb/key/root" ]] || fail "the root is not printed by both: $(cat "$T/out.a" "$T/out.b")"
(( $(wc -l <"$T/runs") == 2 )) || fail "the library was built more than once (make is called twice for one build): $(wc -l <"$T/runs") calls"
[[ ! -d $T/sb/key.lock ]] || fail "the lock is left behind"
(( bad )) || echo "scaling-gate: ok"
exit "$bad"
