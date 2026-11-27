#!/usr/bin/env bash
# A PostgreSQL function whose signature differs between 17, 18 and 19 is called through its macro in src/core/compat.h (sift_table_index_fetch_begin, sift_index_beginscan, ...); a raw call
# compiles on the version the author builds and fails on another (19 added an argument to table_index_fetch_begin: three packages failed to build). This fails on a raw call of one of those
# functions outside src/core/compat.h, and on construct_array_builtin of FLOAT8OID (17 raises "type 701 not supported": sift_construct_float8_array).
#   scripts/check-compat-calls.sh [DIR]   check the tree under DIR (default: the repo)
#   scripts/check-compat-calls.sh --selftest   a raw call is found, the macro and the comment are not
set -uo pipefail
RAW='table_beginscan|table_beginscan_parallel|table_index_fetch_begin|index_beginscan|subquery_planner|GlobalVisTestIsRemovableXid|pgstat_report_vacuum|HandleParallelMessages|ProcessParallelMessages|TupleDescFinalize|lc_collate_is_c'
# the word boundary keeps sift_table_index_fetch_begin( out; a comment line is not a call
scan() { # DIR [SOURCE_DIR]: the raw calls under DIR/SOURCE_DIR (default: the sources)
	(cd "$1" && grep -rnE --include='*.c' --include='*.h' "\\b($RAW)\\(|\\bconstruct_array_builtin\\(.*\\bFLOAT8OID\\b" "${2:-src}" | grep -v '/core/compat\.h:' | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)')
}
# a tuple descriptor built by hand is finalized before its first use (19 asserts on the one that is not): each source that creates one calls sift_tupdesc_finalize as often
unfinalized() { # DIR
	(cd "$1" && grep -rcE --include='*.c' '\bCreateTemplateTupleDesc\(' "${2:-src}" | awk -F: '$2 > 0' | while IFS=: read -r f n; do
		[[ $(grep -cE '\bsift_tupdesc_finalize\(' "$f") -ge $n ]] || echo "$f: $n CreateTemplateTupleDesc, fewer sift_tupdesc_finalize"
	done)
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/tree/col" "$d/tree/core"
	printf '%s\n' 'f = sift_table_index_fetch_begin(heap);' '/* table_index_fetch_begin(heap) is wrapped */' 'g = sift_index_beginscan(h, i, s, 0);' >"$d/tree/col/ok.c"
	printf '%s\n' '#define sift_table_index_fetch_begin(rel) table_index_fetch_begin(rel)' >"$d/tree/core/compat.h"
	[[ -z $(scan "$d" tree) ]] || { echo "FAIL: the macro, the comment or compat.h is reported: $(scan "$d" tree)"; exit 1; }
	printf '%s\n' 'fetch = table_index_fetch_begin(heap);' >"$d/tree/col/mutant.c"
	[[ $(scan "$d" tree) == 'tree/col/mutant.c:1:'* ]] || { echo "FAIL: a raw call of table_index_fetch_begin is not found"; exit 1; }
	rm "$d/tree/col/mutant.c"; printf '%s\n' 'a = construct_array_builtin(d, 3, INT8OID);' >"$d/tree/col/int8.c"
	[[ -z $(scan "$d" tree) ]] || { echo "FAIL: construct_array_builtin of int8, which 17 knows, is reported"; exit 1; }
	printf '%s\n' 'a = construct_array_builtin(d, 3, FLOAT8OID);' >"$d/tree/col/mutant.c"
	[[ $(scan "$d" tree) == 'tree/col/mutant.c:1:'* ]] || { echo "FAIL: construct_array_builtin of float8, which 17 does not know, is not found"; exit 1; }
	printf '%s\n' 'desc = CreateTemplateTupleDesc(2);' 'sift_tupdesc_finalize(desc);' >"$d/tree/col/finalized.c"
	[[ -z $(unfinalized "$d" tree) ]] || { echo "FAIL: a finalized descriptor is reported: $(unfinalized "$d" tree)"; exit 1; }
	printf '%s\n' 'desc = CreateTemplateTupleDesc(2);' >"$d/tree/col/bare.c"
	[[ $(unfinalized "$d" tree) == *bare.c* ]] || { echo "FAIL: a descriptor that is not finalized is not found"; exit 1; }
	echo "compat-calls selftest ok"; exit 0
fi
bad=$(scan "${1:-$(cd "$(dirname "$0")/.." && pwd)}")$(unfinalized "${1:-$(cd "$(dirname "$0")/.." && pwd)}")
if [[ -n $bad ]]; then
	echo "FAIL: a call of a function that differs between PostgreSQL versions, without its macro of src/core/compat.h (sift_<name>):"
	sed 's/^/  /' <<<"$bad"
	exit 1
fi
echo "compat-calls: no raw call outside src/core/compat.h"
