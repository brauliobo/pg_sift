#!/usr/bin/env bash
# The offset of a byte of a page is computed in one place (core/page.h SIFT_PAYLOAD_*, core/xlog.h): a bitmap or table that spans pages must put its logical byte b on page
# b / payload, and the writer that put a bit at `header + sbidx / 8` of the first page of a bitmap of 134,850 bits wrote into the special space of the page (the incident that made this check).
#   - `/ SIFT_PAGE_PAYLOAD`, `% SIFT_PAGE_PAYLOAD` and `* SIFT_PAGE_PAYLOAD` stay in core/page.h: elsewhere SIFT_PAYLOAD_PAGE, _OFF, _PAGES and _BYTES;
#   - `SiftPageGetData(page)[...]` (a byte of a payload by an index) is read in core/live/live.c only, through sift_live_byte; a record is a struct at an offset the caller checked against the room;
#   - a page changes through sift_xlog_* (core/xlog.h), which refuses a write that reaches the special space: no file but core/xlog.h and the host stubs names GenericXLogStart,
#     GenericXLogRegisterBuffer, GenericXLogFinish or GenericXLogAbort.
#
#   scripts/check-paging.sh            the tracked sources of src/
#   scripts/check-paging.sh --selftest
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

# check FILE...: the lines of the files that break a rule, as FILE:LINE: text (one awk for all the files)
check() {
	(( $# )) || return 0
	awk '
		/^[ \t]*(\/?\*|\/\/)/ { next }
		FILENAME !~ /core\/page\.h$/ && /[\/%*] *\(?SIFT_PAGE_PAYLOAD/ { print FILENAME ":" FNR ": paging arithmetic on SIFT_PAGE_PAYLOAD: use SIFT_PAYLOAD_PAGE/_OFF/_PAGES/_BYTES (core/page.h)"; next }
		FILENAME !~ /core\/live\/live\.c$/ && /SiftPageGetData\([^)]*\)\[/ { print FILENAME ":" FNR ": a payload byte by index: only core/live/live.c reads the bytes of a bitmap, through sift_live_byte"; next }
		FILENAME !~ /core\/xlog\.h$/ && /GenericXLog(Start|RegisterBuffer|Finish|Abort)/ { print FILENAME ":" FNR ": generic WAL not through sift_xlog_* (core/xlog.h)" }
	' "$@"
}

if [[ ${1:-} == --selftest ]]; then
	d=$(mktemp -d)
	trap 'rm -rf "$d"' EXIT
	mkdir -p "$d/core"
	printf 'x = b / SIFT_PAGE_PAYLOAD;\n' >"$d/a.c"
	printf 'x = b %% SIFT_PAGE_PAYLOAD;\n' >"$d/b.c"
	printf 'x = (uint64) n * SIFT_PAGE_PAYLOAD;\n' >"$d/c.c"
	printf 'SiftPageGetData(page)[8 + i / 8] |= 1;\n' >"$d/d.c"
	printf 's = GenericXLogStart(rel);\n' >"$d/e.c"
	printf 'n = Min(len, SIFT_PAGE_PAYLOAD - off);\n/* b / SIFT_PAGE_PAYLOAD in a comment */\nx = SIFT_PAYLOAD_PAGE(b);\nstate = sift_xlog_start(rel);\n' >"$d/ok.c"
	printf 'x = b / SIFT_PAGE_PAYLOAD;\n' >"$d/core/page.h"
	for f in a b c d e; do
		[[ -n $(check "$d/$f.c") ]] || { echo "check-paging: selftest: $f.c passed"; exit 1; }
	done
	[[ -z $(check "$d/ok.c" "$d/core/page.h") ]] || { echo "check-paging: selftest: a clean file or core/page.h was reported"; exit 1; }
	echo "check-paging: selftest ok"
	exit 0
fi

cd "$(dirname "$0")/.."
mapfile -t files < <(git ls-files 'src/*.c' 'src/*.h')
bad=$(check "${files[@]}")
[[ -z $bad ]] || { echo "check-paging:"; echo "$bad"; exit 1; }
echo "check-paging: ok, ${#files[@]} files"
