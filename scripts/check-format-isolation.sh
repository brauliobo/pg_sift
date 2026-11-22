#!/usr/bin/env bash
# One format never reads another's files: the engines of Format 2 (a folder f2/, v6/, granule/, skip/ or scope_v6/) include nothing of Format 1 (a v1/ folder, core/v1_col/,
# the directory-chain store and the segment types of core/store.h), and Format 1 (col/v1/, core/v1_col/, table/v1/, agg/v1/) includes nothing of Format 2. Their shared
# files (core/page_io.h, core/meta_peek.h, the codecs, memory and quarantine of core/) are in neither. A file that needs the other side's code gets it moved to the shared files.
# The table access method is a shell in table/ itself (am.c, dispatch.h, wal.c): it reaches a format through the table of table/dispatch.h and includes no file of table/v1/ or table/f2/.
#   scripts/check-format-isolation.sh [DIR]   check the tree under DIR (default: the repo)
#   scripts/check-format-isolation.sh --selftest
set -uo pipefail
V2='(^|/)(f2|v6|granule|skip|scope_v6)/'
V1='(^|/)(v1|v1_col)/'
TBL_SHELL='^src/table/[^/]+$'
TBL_SHELL_BANNED='#include "table/(v1|f2)/'
V2_BANNED='#include "[^"]*/v1/|#include "core/(store|dirchain|lane|live)\.h"|#include "core/v1_col/|SiftDirSnapshot|SiftColSegment|SiftTombLog|format_major == 1'
V1_BANNED='#include "[^"]*/(f2|v6|scope_v6|granule|skip)/|#include "col/(granule|skip)'
scan() { # DIR
	(cd "$1" && find src -type f \( -name '*.c' -o -name '*.h' \) | LC_ALL=C sort | while read -r f; do
		if [[ $f =~ $V2 ]]; then grep -nE "$V2_BANNED" "$f" | sed "s#^#$f:#"
		elif [[ $f =~ $V1 ]]; then grep -nE "$V1_BANNED" "$f" | sed "s#^#$f:#"
		elif [[ $f =~ $TBL_SHELL ]]; then grep -nE "$TBL_SHELL_BANNED" "$f" | sed "s#^#$f:#"; fi
	done)
}
if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d) && trap 'rm -rf "$d"' EXIT
	f2=col/f2 v1=col/v1
	mkdir -p "$d/src/$f2" "$d/src/$v1"
	printf '%s\n' '#include "core/page_io.h"' >"$d/src/$f2/ok.c"
	printf '%s\n' '#include "col/am.h"' >"$d/src/$v1/ok.c"
	[[ -z $(scan "$d") ]] || { echo "FAIL: a clean tree is reported: $(scan "$d")"; exit 1; }
	printf '%s\n' '#include "col/v1/seg.h"' >"$d/src/$f2/mutant.c"
	[[ $(scan "$d") == "src/$f2/mutant.c:1:"* ]] || { echo "FAIL: a Format 1 include in f2/ is not found"; exit 1; }
	rm "$d/src/$f2/mutant.c"
	printf '%s\n' '#include "col/f2/f2.h"' >"$d/src/$v1/mutant.c"
	[[ $(scan "$d") == "src/$v1/mutant.c:1:"* ]] || { echo "FAIL: a Format 2 include in v1/ is not found"; exit 1; }
	rm "$d/src/$v1/mutant.c"
	tb=table
	mkdir -p "$d/src/$tb"
	printf '%s\n' '#include "table/dispatch.h"' >"$d/src/$tb/ok.c"
	[[ -z $(scan "$d") ]] || { echo "FAIL: a clean shell is reported: $(scan "$d")"; exit 1; }
	printf '%s\n' '#include "table/v1/table_v1.h"' >"$d/src/$tb/mutant.c"
	[[ $(scan "$d") == "src/$tb/mutant.c:1:"* ]] || { echo "FAIL: a Format 1 include in the table shell is not found"; exit 1; }
	echo "format-isolation selftest ok"; exit 0
fi
out=$(scan "${1:-$(cd "$(dirname "$0")/.." && pwd)}")
[[ -z $out ]] || { echo "$out"; echo "format isolation: a Format 1 file includes Format 2 code or the other way round, or the table shell includes a format; move what they need into the shared files of core/ and col/ or the dispatch table, do not include across" >&2; exit 1; }
echo "format isolation ok"
