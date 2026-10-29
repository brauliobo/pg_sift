#!/usr/bin/env bash
# The `assert` lane (docs/SAFETY.md): sift against a PostgreSQL built from source with --enable-cassert --enable-debug
# -DCLOBBER_FREED_MEMORY -DRANDOMIZE_ALLOCATED_MEMORY (scripts/pg-assert.sh builds it once into a cache outside the repo; `make pg-assert`).
# Assertions of the server and of sift (Assert), memory-context checking (a write past the end of a palloc chunk is reported at pfree, where
# ASan sees nothing), freed memory overwritten and new memory randomized run under the regression tests and a few TAP tests.
#
#   scripts/assert.sh [--major 17|18] [TEST...]       (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh)
#
# The default tests are the readers and writers of stored bytes (BM25 store and postings, column codec and index, shredder, table AM), the
# planner paths that deparse or cast nodes (facets, agg_subplan_lut) and the aggregate buffers (agg_numeric_const_cmp), the two tests that reload the
# relcache entry of the index at every lock and catalog read (cache_discard, verify_relcache: debug_discard_caches, which cassert builds have), the default of key_columns, which
# rewrites the options of the index in the catalog (col_key_columns_auto), and
# the TAP tests 001_crash and 003_wal_minimal (SIFT_ASSERT_TAP="" skips them). The lane fails on a failed test, on a TRAP (failed Assert)
# or on a memory-context warning or a catalog write without the lock it expects (missing lock for relation) in any server log, which the tests themselves would not notice. In scripts/ci.sh it is opt-in:
# `SIFT_CI_JOBS=assert scripts/ci.sh quick`. KEEP=1 keeps the scratch directory.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
source "$REPO/scripts/source-snapshot.sh"
PGA=()
[[ ${1:-} != --major ]] || { PGA=(--major "$2"); shift 2; }
TESTS=("$@")
(( ${#TESTS[@]} )) || TESTS=(store_basic store_corrupt index_build_postings phrase_positions verify_corrupt query_errors colcodec_basic col_enc
	col_index_basic col_wbuf shred_values tbl_basic tbl_agg facets agg_numeric_const_cmp agg_subplan_lut
	cache_discard verify_relcache col_key_columns_auto agg_visibility_refusal col_wbuf_swap)
TAPS=(${SIFT_ASSERT_TAP-t/001_crash.pl t/003_wal_minimal.pl})
# the logs of the servers (TAP nodes included) must be clean of what the tests do not check
BAD='TRAP: |detected write past chunk end|problem in alloc set|could not find block containing chunk|pfree called with invalid pointer|invalid memory alloc request|missing lock for relation'

PREFIX=$("$REPO/scripts/pg-assert.sh" "${PGA[@]}")
export PATH=$PREFIX/bin:$PATH
pg_config --configure | grep -q -- --enable-cassert || { echo "assert: $PREFIX has no assertions" >&2; exit 1; }
TMP=${SIFT_TMP:-/tmp/sift-assert-$$}
WORK=$TMP/assert
# the clusters of scripts/ci.sh are made by another initdb
unset SIFT_INITDB_TEMPLATE INITDB_TEMPLATE SANITIZE

rm -rf "$WORK" && mkdir -p "$WORK/tree"
# a private copy: the build writes into the tree and its flavor stamp does not know which server it was built for
copy_git_sources "$REPO" "$WORK/tree"
cd "$WORK/tree"
export SIFT_TMP=$WORK/cluster SIFT_ROOT=$WORK/root SIFT_SUDO=
scripts/pgtest.sh install
rc=0
cleanup() {
	scripts/pgtest.sh stop >/dev/null 2>&1 || true
	# the copy is what the logs name: leave the logs where the lane log can point at them when it failed
	(( rc )) || [[ -n ${KEEP:-} ]] || rm -rf "$WORK"
}
trap cleanup EXIT
scripts/pgtest.sh start
scripts/pgtest.sh regress "${TESTS[@]}" || rc=1
# the quick tier of the TAP tests: sift only (the GIN control needs the gin_index_check of amcheck on PostgreSQL 18) and fewer crash iterations
if (( ! rc && ${#TAPS[@]} )); then SIFT_QUICK=1 scripts/tap.sh "${TAPS[@]}" || rc=1; fi
if grep -rEl "$BAD" "$SIFT_TMP" --include='*.log' 2>/dev/null | head -n 5 | grep .; then
	grep -rEh "$BAD" "$SIFT_TMP" --include='*.log' | head -n 10
	echo "assert: a server log has an assertion failure or a memory-context warning (files above)" >&2
	rc=1
fi
(( rc == 0 )) || { echo "assert: FAILED (scratch directory kept: $WORK)" >&2; exit 1; }
echo "assert: ${#TESTS[@]} regression and ${#TAPS[@]} TAP tests passed on $(pg_config --version) ($PREFIX)"
