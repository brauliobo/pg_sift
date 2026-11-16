#!/usr/bin/env bash
# Damaged index data is raised through core/damage.h and nowhere else: the macro of the site says whether the damage belongs to an index that can leave service, so a raise
# with its own errcode(ERRCODE_INDEX_CORRUPTED) or errcode(ERRCODE_DATA_CORRUPTED) is an error no one classified. Reading the code of a caught error is fine.
#   scripts/check-damage.sh [DIR]   check the tree under DIR (default: the repo)
#   scripts/check-damage.sh --selftest   a raise is found, a read of the SQLSTATE and a comment are not
set -uo pipefail
scan() { # DIR [SOURCE_DIR]
	(cd "$1" && grep -rnE --include='*.c' --include='*.h' 'errcode\(ERRCODE_(INDEX|DATA)_CORRUPTED\)' "${2:-src}" | grep -v '/core/damage\.h:' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)')
}
# A test that damages a read of a sound index (sift_test_seg_flip_at) is not damage of that index: only flip_refused (col/test/set_damage.c) arms the flip, inside the diagnostic scope that records nothing
flips() { # DIR [SOURCE_DIR]
	(cd "$1" && grep -rnE --include='*.c' 'sift_test_seg_flip_at = [^U]' "${2:-src}" | grep -vE '/col/test/(set_damage|test)\.c:')
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/tree/col" "$d/tree/core"
	printf '%s\n' 'if (e->sqlerrcode == ERRCODE_INDEX_CORRUPTED) return;' ' * errcode(ERRCODE_DATA_CORRUPTED) is raised by SIFT_DAMAGE_DATA' >"$d/tree/col/ok.c"
	printf '%s\n' 'SIFT_DAMAGE_RAISE(code, ...) ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), __VA_ARGS__))' >"$d/tree/core/damage.h"
	[[ -z $(scan "$d" tree) ]] || { echo "FAIL: a read of the code, a comment or damage.h is reported: $(scan "$d" tree)"; exit 1; }
	printf '%s\n' 'ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("x")));' >"$d/tree/col/mutant.c"
	[[ $(scan "$d" tree) == 'tree/col/mutant.c:1:'* ]] || { echo "FAIL: a raise is not found"; exit 1; }
	printf '%s\n' 'sift_test_seg_flip_at = off + b;' >"$d/tree/col/flip.c"
	[[ $(flips "$d" tree) == 'tree/col/flip.c:1:'* ]] || { echo "FAIL: a flip armed outside flip_refused is not found"; exit 1; }
	echo "damage selftest ok"; exit 0
fi
root=${1:-$(cd "$(dirname "$0")/.." && pwd)}
armed=$(flips "$root")
if [[ -n $armed ]]; then
	echo "FAIL: a segment byte is flipped outside flip_refused; call it, so the refusal runs in a diagnostic scope and the sound index is not taken out of service:"
	sed 's/^/  /' <<<"$armed"
	exit 1
fi
bad=$(scan "$root")
if [[ -n $bad ]]; then
	echo "FAIL: damage raised with its own errcode; raise it through SIFT_DAMAGE_INDEX / SIFT_DAMAGE_DATA / SIFT_DAMAGE_OTHER_* of core/damage.h so the class of the error is decided at the site, and do not catch the error to carry on:"
	sed 's/^/  /' <<<"$bad"
	exit 1
fi
echo "damage: every corruption error is raised through core/damage.h"
