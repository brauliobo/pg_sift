#!/usr/bin/env bash
# Static analysis of src/ with every analyzer this host has (docs/SAFETY.md): clang-tidy (clang-analyzer, bugprone, cert), the
# compilers' own analyzers (gcc -fanalyzer, scan-build), cppcheck. Needs the PostgreSQL server headers (pg_config), nothing else.
#
#   scripts/static.sh [--update-baseline] [--tool NAME[,NAME]] [--files "src/a.c src/b.c"]
#
# NAME: tidy, gcc, scan, cppcheck (a tool that is not installed is reported and skipped). A finding is normalized to
# `tool|check|file|message` (no line or column: they drift with every edit; message text and identifiers stay), and compared with
# scripts/static.baseline, the accepted findings. The new ones are printed. Non-blocking at first: the exit status is 0 unless
# SIFT_STATIC_STRICT=1 and there are new findings. --update-baseline rewrites the baseline to what was found now (review the diff:
# a finding enters the baseline only when it is understood and accepted, or fixed in the phase-2 list of docs/SAFETY.md).
# Runs at nice 19 with at most four processes; output and scratch files in $SIFT_TMP/static (default /tmp/sift-static-PID), removed at the end.
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
BASELINE=$REPO/scripts/static.baseline
UPDATE=0 TOOLS=tidy,gcc,scan,cppcheck FILES=
while (( $# )); do
	case $1 in
		--update-baseline) UPDATE=1 ;;
		--tool) TOOLS=$2; shift ;;
		--files) FILES=$2; shift ;;
		*) sed -n '2,15p' "$0"; exit 1 ;;
	esac
	shift
done
renice -n 19 $$ >/dev/null
WORK=$(mktemp -d "${SIFT_TMP:-/tmp}/sift-static-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
PGINC=$(pg_config --includedir-server)
INC=(); for d in "$REPO"/src/*/; do INC+=(-I"${d%/}"); done
CFLAGS=(-std=gnu11 "${INC[@]}" -isystem "$PGINC" -isystem "$(pg_config --includedir-internal 2>/dev/null || echo "$PGINC")" -DSIFT_HAVE_LZ4 -DSIFT_HAVE_ZSTD -D_GNU_SOURCE -march=x86-64)
[[ -n $FILES ]] || FILES=$(cd "$REPO" && ls src/*/*.c | grep -v -E '_test\.c$')
mapfile -t SRCS < <(tr ' ' '\n' <<<"$FILES" | sed '/^$/d')

# the checks of clang-tidy: the analyzer's core, security and unix families, and the bugprone and cert checks that find overflow,
# truncation and misuse of lengths; the style families (readability, modernize, easily-swappable-parameters) are not safety
TIDY_CHECKS='-*,clang-analyzer-core.*,clang-analyzer-security.*,clang-analyzer-unix.*,clang-analyzer-deadcode.*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-analyzer-security.insecureAPI.strcpy,bugprone-sizeof-expression,bugprone-integer-division,bugprone-suspicious-memset-usage,bugprone-not-null-terminated-result,bugprone-signed-char-misuse,bugprone-misplaced-widening-cast,bugprone-implicit-widening-of-multiplication-result,bugprone-too-small-loop-variable,bugprone-suspicious-memory-comparison,bugprone-unsafe-functions,bugprone-sizeof-container,bugprone-string-literal-with-embedded-nul,cert-arr39-c,cert-str34-c,cert-flp30-c,cert-exp42-c,cert-err34-c,cert-pos44-c'

have() { command -v "$1" >/dev/null 2>&1; }
jobs4() { xargs -P4 "$@"; }

tidy() {
	have clang-tidy || { echo "static: clang-tidy not installed, skipped" >&2; return; }
	printf '%s\n' "${SRCS[@]}" | jobs4 -I{} sh -c 'cd "$1" && clang-tidy -quiet -header-filter="$1/src/.*" -checks="$2" "$3" -- "${@:4}" 2>/dev/null' _ "$REPO" "$TIDY_CHECKS" {} "${CFLAGS[@]}" |
		sed -nE 's/^(.*):[0-9]+:[0-9]+: warning: (.*) \[([a-zA-Z0-9.,_-]+)\]$/tidy|\3|\1|\2/p'
}

gcc_analyzer() {
	have gcc || return
	gcc -fanalyzer -x c /dev/null -fsyntax-only 2>/dev/null || { echo "static: this gcc has no -fanalyzer, skipped" >&2; return; }
	printf '%s\n' "${SRCS[@]}" | jobs4 -I{} sh -c 'cd "$1" && gcc -fanalyzer -O0 -c -o /dev/null -w -Wanalyzer-too-complex "${@:3}" "$2" 2>&1' _ "$REPO" {} "${CFLAGS[@]}" |
		sed -nE 's/^(.*):[0-9]+:[0-9]+: warning: (.*) \[-W(analyzer-[a-zA-Z0-9-]+)\]$/gcc|\3|\1|\2/p'
}

scan_build() {
	have scan-build || { echo "static: scan-build not installed, skipped" >&2; return; }
	( cd "$REPO" && scan-build --status-bugs -o "$WORK/scan" bash -c 'for f in "$@"; do "$CC" -c -o /dev/null $CFLAGS "$f" || true; done' _ "${SRCS[@]}" ) 2>&1 |
		sed -nE 's/^(.*):[0-9]+:[0-9]+: warning: (.*)$/scan|analyzer|\1|\2/p'
}

cppcheck_run() {
	have cppcheck || { echo "static: cppcheck not installed, skipped" >&2; return; }
	( cd "$REPO" && cppcheck -q --enable=warning,portability --inline-suppr --std=c11 "${INC[@]}" --suppress=missingInclude \
		--template='{file}:{line}:{column}: warning: {message} [{id}]' --check-level=normal -j4 "${SRCS[@]}" 2>&1 ) |
		sed -nE 's/^(.*):[0-9]+:[0-9]+: warning: (.*) \[([a-zA-Z0-9_]+)\]$/cppcheck|\3|\1|\2/p'
}

export REPO WORK
export -f have
for t in ${TOOLS//,/ }; do
	case $t in
		tidy) tidy ;;
		gcc) gcc_analyzer ;;
		scan) CC=$(command -v clang || echo cc) CFLAGS="${CFLAGS[*]}" scan_build ;;
		cppcheck) cppcheck_run ;;
		*) echo "static: unknown tool $t" >&2 ;;
	esac
done | sed -E "s#$REPO/##g" | sort -u >"$WORK/found"

echo "static: $(wc -l <"$WORK/found") findings from ${#SRCS[@]} files ($(cut -d'|' -f1 "$WORK/found" | sort | uniq -c | tr -s ' ' | tr '\n' ',' | sed 's/,$//'))"
if (( UPDATE )); then
	{ echo "# Accepted findings of scripts/static.sh: tool|check|file|message, no line numbers. Regenerate with scripts/static.sh --update-baseline."; cat "$WORK/found"; } >"$BASELINE"
	echo "static: baseline written: $BASELINE"
	exit 0
fi
grep -v '^#' "$BASELINE" 2>/dev/null | sort -u >"$WORK/accepted"
comm -23 "$WORK/found" "$WORK/accepted" >"$WORK/new"
comm -13 "$WORK/found" "$WORK/accepted" >"$WORK/fixed"
echo "static: $(wc -l <"$WORK/new") new, $(wc -l <"$WORK/fixed") in the baseline no longer found"
cat "$WORK/new"
[[ ! -s $WORK/new || -z ${SIFT_STATIC_STRICT:-} ]]
