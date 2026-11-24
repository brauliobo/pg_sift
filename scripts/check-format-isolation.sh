#!/usr/bin/env bash
# One format never reads another's files: the engines of Format 2 (a folder f2/, v6/, granule/, skip/ or scope_v6/) include nothing of Format 1 (a v1/ folder, core/v1_col/,
# the directory-chain store and the segment types of core/store.h), and Format 1 (col/v1/, core/v1_col/, table/v1/, agg/v1/) includes nothing of Format 2. Their shared
# files (core/page_io.h, core/meta_peek.h, the codecs, memory and quarantine of core/) are in neither. A file that needs the other side's code gets it moved to the shared files.
# The table access method is a shell in table/ itself (am.c, dispatch.h, wal.c): it reaches a format through the table of table/dispatch.h and includes no file of table/v1/ or table/f2/.
# The aggregate engine (src/agg/) names no format at all: it reaches a store through core/store_ops.h. Only the files of scripts/agg-isolation-v1.allow, the engine of Format 1 that
# is being deleted, are exempt; an entry that is clean or gone fails, so the list only shrinks.
#   scripts/check-format-isolation.sh [DIR]   check the tree under DIR (default: the repo)
#   scripts/check-format-isolation.sh --selftest
set -uo pipefail
V2='(^|/)(f2|v6|granule|skip|scope_v6)/'
V1='(^|/)(v1|v1_col)/'
TBL_SHELL='^src/table/[^/]+$'
AGG='^src/agg/'
AGG_ALLOW=scripts/agg-isolation-v1.allow
TBL_SHELL_BANNED='#include "table/(v1|f2)/'
V2_BANNED='#include "[^"]*/v1/|#include "core/(store|dirchain|lane|live)\.h"|#include "core/v1_col/|SiftDirSnapshot|SiftColSegment|SiftTombLog|format_major == 1'
V1_BANNED='#include "[^"]*/(f2|v6|scope_v6|granule|skip)/|#include "col/(granule|skip)'
AGG_BANNED='#include "col/(f2|v1|granule|skip|scope_v6)/|#include "table/(f2|v1)/|SiftF2Store|SiftColSegment|SiftDirSnapshot|format_major'
agg_allowed() { # DIR
	[[ -f $1/$AGG_ALLOW ]] && grep -vE '^(#|$)' "$1/$AGG_ALLOW" | LC_ALL=C sort
}
agg_stale() { # DIR: the entries of the allow list that name no file or a file that is clean
	agg_allowed "$1" | while read -r f; do
		[[ -f $1/$f ]] || { echo "$AGG_ALLOW: $f: the file is gone, remove its entry"; continue; }
		grep -qE "$AGG_BANNED" "$1/$f" || echo "$AGG_ALLOW: $f: the file is clean, remove its entry"
	done
}
scan() { # DIR
	local allowed; allowed=$(agg_allowed "$1")
	(cd "$1" && {
		local f v2=() v1=() tb=() ag=()
		while read -r f; do
			if [[ $f =~ $V2 ]]; then v2+=("$f")
			elif [[ $f =~ $V1 ]]; then v1+=("$f")
			elif [[ $f =~ $TBL_SHELL ]]; then tb+=("$f"); fi
			if [[ $f =~ $AGG ]] && ! grep -qxF "$f" <<<"$allowed"; then ag+=("$f"); fi
		done < <(find src -type f \( -name '*.c' -o -name '*.h' \) | LC_ALL=C sort)
		{
			(( ${#v2[@]} )) && grep -HnE "$V2_BANNED" "${v2[@]}"
			(( ${#v1[@]} )) && grep -HnE "$V1_BANNED" "${v1[@]}"
			(( ${#tb[@]} )) && grep -HnE "$TBL_SHELL_BANNED" "${tb[@]}"
			(( ${#ag[@]} )) && grep -HnE "$AGG_BANNED" "${ag[@]}"
		} | LC_ALL=C sort -t: -k1,1 -s | awk '!seen[$0]++'
	})
	agg_stale "$1"
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	fail() { echo "FAIL: $*"; exit 1; }
	f2=col/f2 v1=col/v1
	mkdir -p "$d/src/$f2" "$d/src/$v1"
	printf '%s\n' '#include "core/page_io.h"' >"$d/src/$f2/ok.c"
	printf '%s\n' '#include "col/am.h"' >"$d/src/$v1/ok.c"
	[[ -z $(scan "$d") ]] || fail "a clean tree is reported: $(scan "$d")"
	printf '%s\n' '#include "col/v1/seg.h"' >"$d/src/$f2/mutant.c"
	[[ $(scan "$d") == "src/$f2/mutant.c:1:"* ]] || fail "a Format 1 include in f2/ is not found"
	rm "$d/src/$f2/mutant.c"
	printf '%s\n' '#include "col/f2/f2.h"' >"$d/src/$v1/mutant.c"
	[[ $(scan "$d") == "src/$v1/mutant.c:1:"* ]] || fail "a Format 2 include in v1/ is not found"
	rm "$d/src/$v1/mutant.c"
	tb=table
	mkdir -p "$d/src/$tb"
	printf '%s\n' '#include "table/dispatch.h"' >"$d/src/$tb/ok.c"
	[[ -z $(scan "$d") ]] || fail "a clean shell is reported: $(scan "$d")"
	printf '%s\n' '#include "table/v1/table_v1.h"' >"$d/src/$tb/mutant.c"
	[[ $(scan "$d") == "src/$tb/mutant.c:1:"* ]] || fail "a Format 1 include in the table shell is not found"
	rm "$d/src/$tb/mutant.c"
	ag=agg
	mkdir -p "$d/src/$ag/cte" "$d/src/$ag/v6" "$d/src/$ag/plan" "$d/scripts"
	printf '%s\n' '#include "core/store_ops.h"' >"$d/src/$ag/cte/ok.c"
	printf '%s\n' '#include "core/simd_kern.h"' >"$d/src/$ag/v6/ok.c"
	[[ -z $(scan "$d") ]] || fail "a clean src/agg is reported: $(scan "$d")"
	for sub in $ag/cte $ag/v6 $ag $ag/plan; do
		mkdir -p "$d/src/$sub"
		for bad in '#include "col/f2/f2.h"' '#include "col/v1/kern.h"' '#include "col/granule/granule.h"' '#include "col/skip/skip.h"' \
			'#include "col/scope_v6/scope.h"' '#include "table/f2/tbl.h"' '#include "table/v1/src.h"' \
			'int x = sift_col_v2_ops.format_major;' 'static SiftF2Store *st;' 'SiftColSegment *seg;' 'SiftDirSnapshot *dir;'; do
			printf '%s\n' "$bad" >"$d/src/$sub/mutant.c"
			[[ $(scan "$d") == "src/$sub/mutant.c:1:"* ]] || fail "'$bad' in src/$sub is not found"
			[[ $(scan "$d" | wc -l) -eq 1 ]] || fail "'$bad' in src/$sub is reported more than once"
		done
		rm "$d/src/$sub/mutant.c"
	done
	printf '%s\n' '#include "col/v1/conn.h"' >"$d/src/$ag/plan/legacy.c"
	printf '%s\n' '# the engine of Format 1' "src/$ag/plan/legacy.c" >"$d/$AGG_ALLOW"
	[[ -z $(scan "$d") ]] || fail "a listed legacy file is reported: $(scan "$d")"
	printf '%s\n' '#include "col/v1/conn.h"' >"$d/src/$ag/plan/other.c"
	[[ $(scan "$d") == "src/$ag/plan/other.c:1:"* ]] || fail "a legacy file that is not listed is exempt"
	rm "$d/src/$ag/plan/other.c"
	printf '%s\n' 'int clean;' >"$d/src/$ag/plan/legacy.c"
	[[ $(scan "$d") == *"legacy.c: the file is clean"* ]] || fail "a clean entry of the allow list is kept"
	rm "$d/src/$ag/plan/legacy.c"
	[[ $(scan "$d") == *"legacy.c: the file is gone"* ]] || fail "an entry of a deleted file is kept"
	echo "format-isolation selftest ok"; exit 0
fi
out=$(scan "${1:-$(cd "$(dirname "$0")/.." && pwd)}")
[[ -z $out ]] || { echo "$out"; echo "format isolation: a Format 1 file includes Format 2 code or the other way round, the table shell includes a format, or the aggregate engine names a format; move what they need into the shared files of core/ and col/, the dispatch table or core/store_ops.h, do not include across" >&2; exit 1; }
echo "format isolation ok"
