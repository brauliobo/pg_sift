#!/usr/bin/env bash
# A PostgreSQL function whose signature differs between 17, 18 and 19 is called through its macro in src/core/compat.h (sift_table_index_fetch_begin, sift_index_beginscan, ...); a raw call
# compiles on the version the author builds and fails on another (19 added an argument to table_index_fetch_begin: three packages failed to build). This fails on a raw call of one of those
# functions outside src/core/compat.h.
#   scripts/check-compat-calls.sh [DIR]   check the tree under DIR (default: the repo)
#   scripts/check-compat-calls.sh --selftest   a raw call is found, the macro and the comment are not
set -uo pipefail
RAW='table_beginscan|table_beginscan_parallel|table_index_fetch_begin|index_beginscan|subquery_planner|GlobalVisTestIsRemovableXid|pgstat_report_vacuum|HandleParallelMessages|ProcessParallelMessages|TupleDescFinalize|lc_collate_is_c'
# the word boundary keeps sift_table_index_fetch_begin( out; a comment line is not a call
scan() { # DIR [SOURCE_DIR]: the raw calls under DIR/SOURCE_DIR (default: the sources)
	(cd "$1" && grep -rnE --include='*.c' --include='*.h' "\\b($RAW)\\(" "${2:-src}" | grep -v '/core/compat\.h:' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)')
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/tree/col" "$d/tree/core"
	printf '%s\n' 'f = sift_table_index_fetch_begin(heap);' '/* table_index_fetch_begin(heap) is wrapped */' 'g = sift_index_beginscan(h, i, s, 0);' >"$d/tree/col/ok.c"
	printf '%s\n' '#define sift_table_index_fetch_begin(rel) table_index_fetch_begin(rel)' >"$d/tree/core/compat.h"
	[[ -z $(scan "$d" tree) ]] || { echo "FAIL: the macro, the comment or compat.h is reported: $(scan "$d" tree)"; exit 1; }
	printf '%s\n' 'fetch = table_index_fetch_begin(heap);' >"$d/tree/col/mutant.c"
	[[ $(scan "$d" tree) == 'tree/col/mutant.c:1:'* ]] || { echo "FAIL: a raw call of table_index_fetch_begin is not found"; exit 1; }
	echo "compat-calls selftest ok"; exit 0
fi
bad=$(scan "${1:-$(cd "$(dirname "$0")/.." && pwd)}")
if [[ -n $bad ]]; then
	echo "FAIL: a call of a function that differs between PostgreSQL versions, without its macro of src/core/compat.h (sift_<name>):"
	sed 's/^/  /' <<<"$bad"
	exit 1
fi
echo "compat-calls: no raw call outside src/core/compat.h"
