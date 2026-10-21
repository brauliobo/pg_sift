#!/usr/bin/env bash
# The `valgrind` lane (docs/SAFETY.md): memcheck over the server and sift while regression tests run. The PostgreSQL is the source build of
# scripts/pg-assert.sh --valgrind (assertions, -DUSE_VALGRIND: the memory contexts tell valgrind about every palloc chunk, so an overrun
# inside a chunk, a read of uninitialized palloc memory and a use after pfree are errors) with the server's own src/tools/valgrind.supp.
#
#   scripts/valgrind.sh [--major 17|18] [TEST...]       (SIFT_TMP and SIFT_PORT as scripts/pgtest.sh)
#
# Memcheck is 20 to 50 times slower, so the default tests are a representative few: BM25 store, column index, table AM and the shredder
# (`col_enc` alone takes over 30 minutes: name it to run it). The whole run is bounded by SIFT_VALGRIND_TIMEOUT seconds (default 3000; a run that hits it fails). The postmaster and every
# backend run under valgrind with --error-exitcode=99 (a backend with an error exits 99 and the postmaster restarts: the test fails) and
# the logs ($WORK/vg/PID.log) are scanned for error markers, so that an error in a process that did not affect a test still fails the lane.
# SIFT_VALGRIND_ORIGINS=1 adds --track-origins=yes (two times slower, tells where an uninitialized value came from). sift.simd is capped to
# avx2 (SIFT_VALGRIND_SIMD): memcheck has no AVX-512. KEEP=1 keeps the scratch directory.
# In scripts/ci.sh it is opt-in: `SIFT_CI_JOBS=valgrind scripts/ci.sh quick`.
#
# Native or container. Valgrind needs the debug symbols of the dynamic linker (libc6-dbg, glibc-debuginfo); a host whose glibc is a rebuild
# that no debuginfod serves cannot run it. Then the lane runs in a container (SIFT_VALGRIND_CONTAINER=1 forces it, 0 forbids it; by default it
# is chosen when the host probe fails and docker works): a Debian bookworm image (pinned by digest) with valgrind, libc6-dbg and the build
# dependencies; the PostgreSQL is built inside it, with the same flags, into the cache $SIFT_VALGRIND_CACHE (default
# ${XDG_CACHE_HOME:-~/.cache}/sift/pg-valgrind-bookworm, a host binary would not run against the container's glibc); the tests run in it
# as the invoking user's uid on a private copy of the checkout mounted read-only (the checkout itself is never mounted), writing only to the
# scratch directory (SIFT_VALGRIND_CPUS, SIFT_VALGRIND_MEMORY bound the container: 4, 16g), without network. The image sift-valgrind:bookworm-UID is part of the cache (docker rmi to drop it). Without a working
# valgrind and without docker the lane says so and exits 0 (skipped); SIFT_VALGRIND_DEBUGPATH is valgrind's --extra-debuginfo-path.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
source "$REPO/scripts/source-snapshot.sh"
PGA=()
[[ ${1:-} != --major ]] || { PGA=(--major "$2"); shift 2; }
TESTS=("$@")
(( ${#TESTS[@]} )) || read -ra TESTS <<<"${SIFT_VALGRIND_TESTS:-}"
(( ${#TESTS[@]} )) || TESTS=(store_basic col_index_basic tbl_basic shred_values)

BASE_IMAGE=debian:bookworm@sha256:2c037a04925515fdd6ea85ea14a682d0e79931f5e9f5d07b6dbfc6ba12f9e858

# the image: the build dependencies of PostgreSQL and sift, valgrind with the symbols of the dynamic linker, the invoking user's uid
image() {
	docker image inspect "$1" >/dev/null 2>&1 && return
	docker build -q -t "$1" --build-arg UID="$(id -u)" - <<DOCKERFILE >/dev/null
FROM $BASE_IMAGE
RUN apt-get update && apt-get install -y --no-install-recommends build-essential bison flex curl ca-certificates bzip2 perl libipc-run-perl \\
	liblz4-dev libzstd-dev valgrind libc6-dbg git util-linux procps && rm -rf /var/lib/apt/lists/*
ARG UID
RUN useradd -m -u \$UID sift
DOCKERFILE
}

# container [TEST...]: the lane in the image; the checkout is copied (git's tracked and untracked files) to a read-only tree
container() {
	tmp=${SIFT_TMP:-/tmp/sift-valgrind-$$}/container name=sift-valgrind-$$-$RANDOM rc=0
	local uid image
	local cache=${SIFT_VALGRIND_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/sift/pg-valgrind-bookworm}
	uid=$(id -u) image=sift-valgrind:bookworm-$uid
	image "$image"
	rm -rf "$tmp" && mkdir -p "$tmp/export" "$tmp/scratch" "$cache"
	trap 'docker rm -f "$name" >/dev/null 2>&1 || true; (( rc )) || [[ -n ${KEEP:-} ]] || rm -rf "$tmp"' EXIT
	copy_git_sources "$REPO" "$tmp/export"
	local run=(docker run --rm --init --name "$name" --user "$uid" --cpus "${SIFT_VALGRIND_CPUS:-4}" --memory "${SIFT_VALGRIND_MEMORY:-16g}" -e HOME=/tmp -e SIFT_PG_CACHE=/cache
		-v "$tmp/export:/src:ro" -v "$cache:/cache")
	# the server build needs the network the first time (the tarball), the tests do not
	"${run[@]}" "$image" /src/scripts/pg-assert.sh --valgrind "${PGA[@]}" >/dev/null || { rc=1; echo "valgrind: the PostgreSQL build in the container failed"; return 1; }
	"${run[@]}" --network none -v "$tmp/scratch:/scratch" -e SIFT_TMP=/scratch -e SIFT_VALGRIND_INSIDE=1 -e KEEP="${KEEP:-}" \
		${SIFT_VALGRIND_ORIGINS:+-e SIFT_VALGRIND_ORIGINS=1} ${SIFT_VALGRIND_SIMD:+-e SIFT_VALGRIND_SIMD="$SIFT_VALGRIND_SIMD"} \
		${SIFT_VALGRIND_TIMEOUT:+-e SIFT_VALGRIND_TIMEOUT="$SIFT_VALGRIND_TIMEOUT"} "$image" /src/scripts/valgrind.sh "${PGA[@]}" "${TESTS[@]}" || rc=$?
	(( rc == 0 )) || echo "valgrind: FAILED in the container (scratch directory kept: $tmp/scratch)"
	return $rc
}

DBG=(${SIFT_VALGRIND_DEBUGPATH:+--extra-debuginfo-path="$SIFT_VALGRIND_DEBUGPATH"})
# an installed valgrind that cannot start any program is not usable on this host
host_valgrind() { command -v valgrind >/dev/null && valgrind --quiet "${DBG[@]}" /bin/true >/dev/null 2>&1; }

if [[ -z ${SIFT_VALGRIND_INSIDE:-} && ${SIFT_VALGRIND_CONTAINER:-auto} != 0 ]]; then
	if [[ ${SIFT_VALGRIND_CONTAINER:-auto} == 1 ]] || ! host_valgrind; then
		if command -v docker >/dev/null && docker info >/dev/null 2>&1; then container; exit $?; fi
		[[ ${SIFT_VALGRIND_CONTAINER:-auto} != 1 ]] || { echo "valgrind: SIFT_VALGRIND_CONTAINER=1 but docker does not work" >&2; exit 1; }
	fi
fi
host_valgrind || { echo "valgrind: skipped, valgrind is missing or cannot run here (glibc debug symbols: libc6-dbg, glibc-debuginfo, SIFT_VALGRIND_DEBUGPATH; or docker for the container)"; exit 0; }

PREFIX=$("$REPO/scripts/pg-assert.sh" --valgrind "${PGA[@]}")
export PATH=$PREFIX/bin:$PATH
TMP=${SIFT_TMP:-/tmp/sift-valgrind-$$}
WORK=$TMP/valgrind
unset SIFT_INITDB_TEMPLATE INITDB_TEMPLATE SANITIZE
rm -rf "$WORK" && mkdir -p "$WORK/tree" "$WORK/vg"
# a private copy; in the container the checkout is already an export without .git
if [[ -e $REPO/.git ]]; then copy_git_sources "$REPO" "$WORK/tree"
else tar -C "$REPO" -cf - . | tar -C "$WORK/tree" -xf -; fi
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

# one pg_regress per test: the seconds of each are printed (they size the default list), and the bound covers the whole loop
t0=$SECONDS
for t in "${TESTS[@]}"; do
	left=$(( ${SIFT_VALGRIND_TIMEOUT:-3000} - (SECONDS - t0) ))
	t1=$SECONDS
	if (( left > 0 )) && timeout -k 60 "$left" scripts/pgtest.sh regress "$t"; then
		echo "valgrind: test $t $((SECONDS - t1)) s"
	else
		rc=1; echo "valgrind: test $t failed or the ${SIFT_VALGRIND_TIMEOUT:-3000} s bound was hit after $((SECONDS - t1)) s"
	fi
done
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
