#!/usr/bin/env bash
# The deterministic scaling gate: no clock is read, so it does not flake under load. The cost of a data structure is its count of operations at n and at 8n, the cost of a statement is the
# buffers it touches at 2,000 and at 16,000 rows (Spec 66).
#   C     test/scaling/check_scaling_c.c runs the real code of src/ and sqlite/host under compiler instrumentation (op_counters.h) and fails a ratio above the ceiling of its class
#   SQL   test/sql/scaling_ops_gate.sql: shared blocks, temp blocks and the nodes of the plan of eight shapes, through pg_regress
# A gate marked pending in either names the lane that fixes the code: it must stay over its ceiling until then, and fails as stale once it does not.
#
#   scripts/check-scaling-ops.sh               the C gate plain and under ASan+UBSan, then the SQL gate
#   scripts/check-scaling-ops.sh --c-only      the C gate; --asan builds it with AddressSanitizer and UBSan (no PostgreSQL needed, ELF and GNU ld: Linux)
#   scripts/check-scaling-ops.sh --sql-only    the SQL gate against the cluster of SIFT_TMP and SIFT_PORT (scripts/pgtest.sh start), or a cluster of its own when none runs
#   scripts/check-scaling-ops.sh --selftest    a planted O(n^2) loop and a stale pending marker must fail the C gate; the expected output of the SQL gate has no failing row
# SIFT_SCALING_OUT=DIR keeps the build of the C gate between runs.
set -euo pipefail
cd "$(dirname "$0")/.."

tmp=$(mktemp -d)
cleanup() {
	[[ -z ${own_cluster:-} ]] || SIFT_TMP=$own_cluster scripts/pgtest.sh destroy >/dev/null 2>&1 || true
	rm -rf "$tmp"
}
trap cleanup EXIT

# c_build SAN: the binary of the C gate, built into $c_out
c_build() {
	local flavor=${1:+-san}
	c_out=${SIFT_SCALING_OUT:-$tmp}/c$flavor
	nice make -s -j4 -C test/scaling OUT="$c_out" ${1:+SAN=1} >/dev/null
}

c_gate() {
	c_build "${1:-}"
	"$c_out/check_scaling_c"
}

sql_gate() {
	local sock=${SIFT_TMP:-/tmp/sift-arch}/sock port=${SIFT_PORT:-56100}
	if ! pg_isready -q -h "$sock" -p "$port"; then
		own_cluster=$tmp/pg
		export SIFT_TMP=$own_cluster SIFT_PORT=$((56100 + RANDOM % 100))
		scripts/pgtest.sh install >/dev/null
		scripts/pgtest.sh start >/dev/null
	fi
	scripts/pgtest.sh regress scaling_ops_gate
}

expected_has_no_failing_row() {
	! grep -nE '^ (FAIL|STALE) \|' test/expected/scaling_ops_gate.out
}

selftest() {
	c_build ""
	"$c_out/check_scaling_c" --negative-test
	expected_has_no_failing_row
	echo "check-scaling-ops: selftest ok"
}

case ${1:---all} in
--all) c_gate ""; c_gate san; sql_gate ;;
--c-only) c_gate "${2:+san}" ;;
--sql-only) sql_gate ;;
--selftest) selftest ;;
*) echo "usage: scripts/check-scaling-ops.sh [--all | --c-only [--asan] | --sql-only | --selftest]" >&2; exit 2 ;;
esac
