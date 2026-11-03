#!/usr/bin/env bash
# pgtest.sh ready (and clean, which waits for it) holds a job back while the cluster of the lane recovers from a crashed backend, and fails when it never accepts connections.
# pg_isready is a stub that refuses its first N calls. Seconds, no server.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
bad=0
fail() { echo "FAIL: $*"; bad=1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/bin"
cat >"$TMP/bin/pg_isready" <<'EOF'
#!/usr/bin/env bash
n=$(cat "$STUB_DIR/calls" 2>/dev/null || echo 0)
echo $((n + 1)) >"$STUB_DIR/calls"
(( n + 1 > ${STUB_REFUSE:-0} ))
EOF
chmod +x "$TMP/bin/pg_isready"
# ready SECS REFUSE: the output is "rc calls"
ready_after() {
	rm -f "$TMP/calls"
	PATH=$TMP/bin:$PATH STUB_DIR=$TMP STUB_REFUSE=$2 SIFT_READY_SECS=$1 SOCK=$TMP PORT=1 "$BASH" -c '
		eval "$(sed -n "/^ready() {/,/^}/p" "$1/scripts/pgtest.sh")"
		ready 2>/dev/null && echo "0 $(cat "$STUB_DIR/calls")" || echo "1 $(cat "$STUB_DIR/calls")"' _ "$REPO"
}
[[ $(ready_after 5 0) == "0 1" ]] || fail "a cluster that accepts connections is waited for: $(ready_after 5 0)"
[[ $(ready_after 5 3) == "0 4" ]] || fail "a cluster in recovery is waited for until it accepts: $(ready_after 5 3)"
[[ $(ready_after 1 99) == "1 2" ]] || fail "a cluster that never accepts fails after SIFT_READY_SECS: $(ready_after 1 99)"
grep -q 'ready || return 1' <(sed -n '/^clean() {/,/^}/p' "$REPO/scripts/pgtest.sh") || fail "clean does not wait for the cluster"
grep -q 'pgtest.sh" clean' <(sed -n '/^lane() {/,/^}/p' "$REPO/scripts/ci/lane.sh") || fail "a lane does not clean after a failed job"
(( bad )) || echo "ci-ready: ok"
exit "$bad"
