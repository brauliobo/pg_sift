#!/usr/bin/env bash
# The `valgrind` lane (docs/SAFETY.md): memcheck over the server and sift while regression tests run. The PostgreSQL is the source build of
# scripts/pg-assert.sh --valgrind (assertions, -DUSE_VALGRIND: the memory contexts tell valgrind about every palloc chunk, so an overrun
# inside a chunk, a read of uninitialized palloc memory and a use after pfree are errors) with the server's own src/tools/valgrind.supp.
#
#   scripts/valgrind.sh [--major 17|18] [TEST...]       (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh)
#
# Memcheck is 20 to 50 times slower, so the default tests are a representative few: BM25 store, column encodings, column index, table AM and the
# shredder. The whole run is bounded by SIFT_VALGRIND_TIMEOUT seconds (default 3000; a run that hits it fails). The postmaster and every
# backend run under valgrind with --error-exitcode=99 (a backend with an error exits 99 and the postmaster restarts: the test fails) and
# the logs ($WORK/vg/PID.log) are scanned for error markers, so that an error in a process that did not affect a test still fails the lane.
# SIFT_VALGRIND_ORIGINS=1 adds --track-origins=yes (two times slower, tells where an uninitialized value came from). sift.simd is capped to
# avx2 (SIFT_VALGRIND_SIMD): memcheck has no AVX-512. Without a working valgrind the lane says so and exits 0 (skipped); SIFT_VALGRIND_DEBUGPATH is
# valgrind's --extra-debuginfo-path for the glibc symbols. KEEP=1 keeps the
# scratch directory. In scripts/ci.sh it is opt-in: `SIFT_CI_JOBS=valgrind scripts/ci.sh quick`.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
command -v valgrind >/dev/null || { echo "valgrind: skipped, valgrind is not installed"; exit 0; }
DBG=(${SIFT_VALGRIND_DEBUGPATH:+--extra-debuginfo-path="$SIFT_VALGRIND_DEBUGPATH"})
# an installed valgrind that cannot start any program (it needs the debug symbols of the dynamic linker: libc6-dbg, glibc-debuginfo) is a skip, too
valgrind --quiet "${DBG[@]}" /bin/true >/dev/null 2>&1 ||
	{ echo "valgrind: skipped, $(valgrind --version) cannot run here (glibc debug symbols missing: install libc6-dbg or glibc-debuginfo, or set SIFT_VALGRIND_DEBUGPATH)"; exit 0; }
PGA=()
[[ ${1:-} != --major ]] || { PGA=(--major "$2"); shift 2; }
TESTS=("$@")
(( ${#TESTS[@]} )) || TESTS=(store_basic col_enc col_index_basic tbl_basic shred_values)

PREFIX=$("$REPO/scripts/pg-assert.sh" --valgrind "${PGA[@]}")
export PATH=$PREFIX/bin:$PATH
TMP=${SIFT_TMP:-/tmp/sift-valgrind-$$}
WORK=$TMP/valgrind
unset SIFT_INITDB_TEMPLATE INITDB_TEMPLATE SANITIZE
rm -rf "$WORK" && mkdir -p "$WORK/tree" "$WORK/vg"
( cd "$REPO" && git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf - ) | tar -C "$WORK/tree" -xf -
cd "$WORK/tree"
export SIFT_TMP=$WORK/cluster SIFT_ROOT=$WORK/root SIFT_SUDO=
PORT=${SIFT_PORT:-56100}
DATA=$SIFT_TMP/data SOCK=$SIFT_TMP/sock
scripts/pgtest.sh install
mkdir -p "$SOCK"
scripts/pgtest.sh new-data "$DATA"
cat >>"$DATA/postgresql.conf" <<EOF
port = $PORT
listen_addresses = ''
unix_socket_directories = '$SOCK'
$(scripts/pgtest.sh conf)
sift.simd = '${SIFT_VALGRIND_SIMD:-avx2}'
fsync = off
shared_buffers = 64MB
max_worker_processes = 8
max_parallel_workers_per_gather = 0
EOF

rc=0
cleanup() {
	pg_ctl -D "$DATA" -m immediate -t 120 stop >/dev/null 2>&1 || true
	(( rc )) || [[ -n ${KEEP:-} ]] || rm -rf "$WORK"
}
trap cleanup EXIT
VG=(valgrind --quiet "${DBG[@]}" --trace-children=yes --leak-check=no --error-exitcode=99 --suppressions="$PREFIX/share/valgrind.supp"
	--error-markers=VALGRINDERROR-BEGIN,VALGRINDERROR-END --log-file="$WORK/vg/%p.log" ${SIFT_VALGRIND_ORIGINS:+--track-origins=yes})
nice "${VG[@]}" postgres -D "$DATA" >"$WORK/postgres.out" 2>&1 &
for _ in $(seq 600); do pg_isready -q -h "$SOCK" -p "$PORT" && break; sleep 1; done
pg_isready -q -h "$SOCK" -p "$PORT" || { echo "valgrind: the server did not start in 600 s"; tail -n 20 "$WORK/postgres.out"; rc=1; exit 1; }
createdb -U postgres "${SIFT_DB:-sift}" 2>/dev/null || true

timeout -k 60 "${SIFT_VALGRIND_TIMEOUT:-3000}" scripts/pgtest.sh regress "${TESTS[@]}" || { rc=1; echo "valgrind: a test failed or the ${SIFT_VALGRIND_TIMEOUT:-3000} s bound was hit"; }
pg_ctl -D "$DATA" -m fast -t 600 stop >/dev/null 2>&1 || true

# every error is a block between the markers in the log of its process
n=$(cat "$WORK"/vg/*.log 2>/dev/null | grep -c VALGRINDERROR-BEGIN || true)
if (( n )); then
	echo "valgrind: $n error(s); first ones:"
	cat "$WORK"/vg/*.log | awk '/VALGRINDERROR-BEGIN/ { c++ } c && c <= 3 { print } /VALGRINDERROR-END/ && c >= 3 { exit }'
	rc=1
fi
(( rc == 0 )) || { echo "valgrind: FAILED (scratch directory kept: $WORK)"; exit 1; }
echo "valgrind: ${#TESTS[@]} regression tests ran clean under memcheck on $(pg_config --version)"
