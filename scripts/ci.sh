#!/usr/bin/env bash
# Single entry point for the test suites, with a pass/fail/skip summary.
#
#   scripts/ci.sh [smoke|full]
#
# smoke (default, a few minutes): build, installcheck, differential test,
#   isolation (emulated and real), TAP wal_level=minimal, soak and fuzz, all
#   in short bounded runs with one client.
# full: ASan build and cluster, installcheck, longer differential test plus
#   the mutation test of the harness, every isolation and TAP test (Perl and
#   shell), all fuzz targets, GIN-control and sift soaks, and the server
#   sanitizer report.
# Everything runs at nice 19 on the scripts/pgtest.sh cluster ($SIFT_TMP,
# $SIFT_PORT), which is stopped at the end. Logs: $SIFT_TMP/ci/<suite>.log.
# Stages whose sift code has not landed are skipped, not failed. Exit 1 if any
# suite fails.
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-smoke}
[[ $MODE == smoke || $MODE == full ]] || { sed -n '2,17p' "$0"; exit 1; }
renice -n 19 $$ >/dev/null
LOG=${SIFT_TMP:-/tmp/sift-arch}/ci
S=$REPO/scripts
rm -rf "$LOG" && mkdir -p "$LOG"
summary=()

suite() {
	local name=$1 t0=$SECONDS status skips
	shift
	printf '== %-18s' "$name"
	if "$@" >"$LOG/$name.log" 2>&1; then status=PASS; else status=FAIL; fi
	skips=$(grep -ciE '\bskip' "$LOG/$name.log" || true)
	summary+=("$(printf '%s %-18s %4ss  %s skip lines' "$status" "$name" $((SECONDS - t0)) "$skips")")
	echo " $status"
}

if [[ $MODE == full ]]; then
	suite build env SANITIZE=1 "$S/pgtest.sh" install
else
	suite build env -u SANITIZE "$S/pgtest.sh" install
fi
"$S/pgtest.sh" stop
"$S/pgtest.sh" start
suite installcheck "$S/pgtest.sh" regress
if [[ $MODE == full ]]; then
	suite difftest "$S/difftest.sh" --docs 5000 --queries 150 --rounds 4
	suite difftest-mutants "$S/difftest.sh" --engine mutants --docs 1500 --queries 40 --rounds 2 --minimize 1
	suite isolation-emulated "$S/isolation.sh" --emulate
	suite isolation "$S/isolation.sh"
	suite tap "$S/tap.sh"
	suite tap-shell "$S/pgtest.sh" tap
	suite soak-gin "$S/soak.sh" --am gin --duration 120 --rows 20000 --clients 2 --check-every 20
	suite soak "$S/soak.sh" --duration 600 --check-every 30
	suite fuzz "$S/fuzz.sh"
else
	suite difftest "$S/difftest.sh" --docs 500 --queries 15 --rounds 1 --minimize 1 --min-trials 10
	suite isolation-emulated "$S/isolation.sh" --emulate cic_insert
	suite isolation "$S/isolation.sh"
	suite tap "$S/tap.sh" t/003_wal_minimal.pl
	suite soak "$S/soak.sh" --duration 20 --rows 2000 --rate 50 --clients 1 --check-every 10
	suite fuzz "$S/fuzz.sh" --scale 1 canary sql
fi
"$S/pgtest.sh" stop

echo
green=$'\e[32m' red=$'\e[31m' off=$'\e[0m'
[[ -t 1 ]] || green= red= off=
failed=0
for line in "${summary[@]}"; do
	[[ $line == FAIL* ]] && { failed=1; echo "$red$line$off"; } || echo "$green$line$off"
done
echo "logs: $LOG"
exit $failed
