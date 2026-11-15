#!/usr/bin/env bash
# Damaged index data is raised through core/damage.h and nowhere else: the macro of the site says whether the damage belongs to an index that can leave service, so a raise
# with its own errcode(ERRCODE_INDEX_CORRUPTED) or errcode(ERRCODE_DATA_CORRUPTED) is an error no one classified. Reading the code of a caught error is fine.
#   scripts/check-damage.sh [DIR]   check the tree under DIR (default: the repo)
#   scripts/check-damage.sh --selftest   a raise is found, a read of the SQLSTATE and a comment are not
set -uo pipefail
scan() { # DIR [SOURCE_DIR]
	(cd "$1" && grep -rnE --include='*.c' --include='*.h' 'errcode\(ERRCODE_(INDEX|DATA)_CORRUPTED\)' "${2:-src}" | grep -v '/core/damage\.h:' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)')
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/tree/col" "$d/tree/core"
	printf '%s\n' 'if (e->sqlerrcode == ERRCODE_INDEX_CORRUPTED) return;' ' * errcode(ERRCODE_DATA_CORRUPTED) is raised by SIFT_DAMAGE_DATA' >"$d/tree/col/ok.c"
	printf '%s\n' 'SIFT_DAMAGE_RAISE(code, ...) ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), __VA_ARGS__))' >"$d/tree/core/damage.h"
	[[ -z $(scan "$d" tree) ]] || { echo "FAIL: a read of the code, a comment or damage.h is reported: $(scan "$d" tree)"; exit 1; }
	printf '%s\n' 'ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("x")));' >"$d/tree/col/mutant.c"
	[[ $(scan "$d" tree) == 'tree/col/mutant.c:1:'* ]] || { echo "FAIL: a raise is not found"; exit 1; }
	echo "damage selftest ok"; exit 0
fi
bad=$(scan "${1:-$(cd "$(dirname "$0")/.." && pwd)}")
if [[ -n $bad ]]; then
	echo "FAIL: damage raised with its own errcode; raise it through SIFT_DAMAGE_INDEX / SIFT_DAMAGE_DATA / SIFT_DAMAGE_OTHER_* of core/damage.h so the class of the error is decided at the site, and do not catch the error to carry on:"
	sed 's/^/  /' <<<"$bad"
	exit 1
fi
echo "damage: every corruption error is raised through core/damage.h"
