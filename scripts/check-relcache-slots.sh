#!/usr/bin/env bash
# A backend has 10 relcache callback slots (MAX_RELCACHE_CALLBACKS) that every loaded library shares, and running out is a FATAL that ends the session: sift takes none (a syscache
# callback has 64 slots of its own; test/t/relcache_slots.sh measures it, on the paths it runs, against the server's). This fails on a CacheRegisterRelcacheCallback call in src/ other than
# the one of the test function that burns slots on purpose (src/core/store_ops_test.c).
#   scripts/check-relcache-slots.sh [DIR]   check the tree under DIR (default: the repo)
#   scripts/check-relcache-slots.sh --selftest   a call is found, a syscache callback and a comment are not
set -uo pipefail
scan() { # DIR [SOURCE_DIR]
	(cd "$1" && grep -rnE --include='*.c' --include='*.h' '\bCacheRegisterRelcacheCallback\(' "${2:-src}" | grep -v '/core/store_ops_test\.c:' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)')
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/tree/col" "$d/tree/core"
	printf '%s\n' 'CacheRegisterSyscacheCallback(RELOID, f, (Datum) 0);' ' * CacheRegisterRelcacheCallback(f, 0) is not taken' >"$d/tree/col/ok.c"
	printf '%s\n' 'CacheRegisterRelcacheCallback(burn_inval, (Datum) 0);' >"$d/tree/core/store_ops_test.c"
	[[ -z $(scan "$d" tree) ]] || { echo "FAIL: a syscache callback, a comment or the burn function is reported: $(scan "$d" tree)"; exit 1; }
	printf '%s\n' 'CacheRegisterRelcacheCallback(inval, (Datum) 0);' >"$d/tree/col/mutant.c"
	[[ $(scan "$d" tree) == 'tree/col/mutant.c:1:'* ]] || { echo "FAIL: a relcache callback is not found"; exit 1; }
	echo "relcache-slots selftest ok"; exit 0
fi
bad=$(scan "${1:-$(cd "$(dirname "$0")/.." && pwd)}")
if [[ -n $bad ]]; then
	echo "FAIL: a relcache callback slot taken (CacheRegisterSyscacheCallback(RELOID, ..) follows pg_class without one):"
	sed 's/^/  /' <<<"$bad"
	exit 1
fi
echo "relcache-slots: sift registers no relcache callback"
