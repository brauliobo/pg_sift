#!/usr/bin/env bash
# Does every optimization the CHANGELOG claims a speed for have a benchmark case, and does every case feed a block and have results? No measurement, no server (python, about a second).
#
#   scripts/check-bench-cases.sh             exit 1 when a CHANGELOG line this change adds under `## Unreleased` claims a speed (a before and after, a ratio, instructions) and no case of
#                                            bench/cases.tsv names it (the `changelog` column: a phrase of the line) and scripts/bench-cases.allow does not waive it; when a case's
#                                            phrase is in no line of CHANGELOG.md, its SQL file or its README/BENCHMARKS block (<!-- bench:BLOCK:begin -->) is missing, or docs/results/cases.tsv
#                                            has no sift and native row of it; when an allow line matches no claim or has no reason. "Adds": the lines of CHANGELOG.md that differ from the merge base
#                                            with origin/integration/3.3-batch-1 (BENCH_CASES_BASE=REF overrides it, origin/main is the fallback; its tip when there is no merge base); without a base only the registry is checked.
#   scripts/check-bench-cases.sh --strict    every claim of Unreleased, not just the new ones: the release gate (docs/RELEASE.md)
#   scripts/check-bench-cases.sh --selftest  proves the gate fails on each kind of gap and passes on a covered tree
# Fix: add bench/cases/ID.sql and a row to bench/cases.tsv, run `bench/bench cases --cases ID --docs`; or a line `phrase<TAB>reason` to scripts/bench-cases.allow for a claim no query measures.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
CASES=$R/bench/steps/cases.py

if [[ ${1:-} == --selftest ]]; then
	T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
	fixture() { # DIR: a tree that passes
		mkdir -p "$1/bench/cases" "$1/docs/results" "$1/scripts"
		printf 'a\tphrase one\torders\tdefault\tSiftAgg\tnative\t-\tms\tfast\tyes\n' >"$1/bench/cases.tsv"
		printf -- '-- label: one\n--@ pg\nSELECT 1\n' >"$1/bench/cases/a.sql"
		printf '## Unreleased\n\n- Added: pg_sift: phrase one is 10 ms against 1 ms.\n- Fixed: pg_sift: a crash.\n\n## 3.2\n\n- Changed: pg_sift: old phrase was 5 ms to 4 ms.\n' >"$1/CHANGELOG.md"
		printf '<!-- bench:fast:begin -->\n<!-- bench:fast:end -->\n' >"$1/README.md"
		printf '# t\nversion\tcase\tarm\tmetric\tvalue\tmin\tmax\tn\tload\tnote\n3.3\ta\tnative\tms\t10\t1\t1\t1\t1\t\n3.3\ta\tsift\tms\t1\t1\t1\t1\t1\t\n' >"$1/docs/results/cases.tsv"
	}
	expect() { # WHAT FAIL|PASS DIR [ARGS]: the check's verdict and, for a failure, the words it says
		local what=$1 want=$2 dir=$3 words=$4; shift 4
		local out rc=0
		out=$(python3 -B -I "$CASES" check --root "$dir" "$@" 2>&1) || rc=$?
		if [[ $want == PASS ]]; then (( rc == 0 )) || { echo "selftest: $what should pass:"; echo "$out"; exit 1; }
		else { (( rc != 0 )) && grep -q -- "$words" <<<"$out"; } || { echo "selftest: $what should fail with '$words' (exit $rc):"; echo "$out"; exit 1; }; fi
	}
	fixture "$T/ok"; expect "a covered tree" PASS "$T/ok" ""; expect "a covered tree, strict" PASS "$T/ok" "" --strict
	cp -r "$T/ok" "$T/claim"
	sed -i 's/^- Fixed: pg_sift: a crash.$/&\n- Changed: pg_sift: phrase two went from 8 ms to 2 ms./' "$T/claim/CHANGELOG.md"
	expect "a new speed claim with no case" FAIL "$T/claim" "claims a speed with no case" --changed <(printf -- '- Changed: pg_sift: phrase two went from 8 ms to 2 ms.\n')
	expect "an old claim with no case, by default" PASS "$T/claim" "" --changed /dev/null
	expect "an old claim with no case, strict" FAIL "$T/claim" "claims a speed with no case" --strict
	printf 'phrase two\ta crash-free storage path, measured by no query\n' >"$T/claim/scripts/bench-cases.allow"
	expect "a claim with a reasoned allow line" PASS "$T/claim" "" --strict
	printf 'phrase two\t\n' >"$T/claim/scripts/bench-cases.allow"; expect "an allow line without a reason" FAIL "$T/claim" "has no reason" --strict
	printf 'phrase nine\tthere is no such line\n' >"$T/claim/scripts/bench-cases.allow"; expect "an allow line that matches nothing" FAIL "$T/claim" "matches no speed claim" --strict
	fixture "$T/anchor"; sed -i 's/phrase one/phrase uno/' "$T/anchor/CHANGELOG.md"; expect "a case whose phrase left the CHANGELOG" FAIL "$T/anchor" "is in no line of CHANGELOG.md"
	fixture "$T/block"; printf '' >"$T/block/README.md"; expect "a case that feeds no block" FAIL "$T/block" "do not have"
	fixture "$T/rows"; sed -i '/\tsift\t/d' "$T/rows/docs/results/cases.tsv"; expect "a case with no result row" FAIL "$T/rows" "has no sift row"
	fixture "$T/orphan"; printf -- '--@ pg\nSELECT 2\n' >"$T/orphan/bench/cases/b.sql"; expect "a SQL file in no row" FAIL "$T/orphan" "is in no row of bench/cases.tsv"
	echo "check-bench-cases: selftest passed (a covered tree passes; a new claim, an old claim in --strict, a moved phrase, a missing block, missing rows, an orphan file and a bad allow line fail)"
	exit 0
fi

STRICT=''; [[ ${1:-} != --strict ]] || STRICT=1
ARGS=(--root "$R")
if [[ -n $STRICT ]]; then
	ARGS+=(--strict)
else
	base=${BENCH_CASES_BASE:-}
	if [[ -z $base ]]; then
		for ref in origin/integration/3.3-batch-1 origin/main; do   # a shallow clone has no merge base: the tip of the branch will do, only the lines this change adds count
			if git -C "$R" rev-parse -q --verify "$ref" >/dev/null 2>&1; then base=$(git -C "$R" merge-base HEAD "$ref" 2>/dev/null || echo "$ref"); break; fi
		done
	fi
	if [[ -z $base ]]; then
		echo "bench-cases: warning: no base to compare CHANGELOG.md with (set BENCH_CASES_BASE, or --strict): only the registry is checked"
		ARGS+=(--changed /dev/null)
	else
		T=$(mktemp); trap 'rm -f "$T"' EXIT
		git -C "$R" diff --no-ext-diff -U0 "$base" -- CHANGELOG.md | sed -n 's/^+\(- .*\)$/\1/p' >"$T"
		ARGS+=(--changed "$T")
	fi
fi
exec python3 -B -I "$CASES" check "${ARGS[@]}"
