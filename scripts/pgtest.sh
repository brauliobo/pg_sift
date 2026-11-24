#!/usr/bin/env bash
# Throwaway PostgreSQL cluster that loads sift from the repo build output, without a
# system-wide install (extension_control_path, dynamic_library_path). PostgreSQL 17 has no
# extension_control_path: install then copies sift into the server's own directories, as
# SIFT_SUDO (e.g. sudo) when those are not writable. Two checkouts that install for PostgreSQL 17 on one machine overwrite each other's library there (a test then fails with an
# option or a behavior of the other checkout): use PostgreSQL 18 when several work on a machine, or install, start and run in one go.
#
#   scripts/pgtest.sh install    build + install into $SIFT_TMP/root
#   scripts/pgtest.sh start      initdb (first time) + start, without autovacuum (see start)
#   scripts/pgtest.sh psql [...] psql into the test database
#   scripts/pgtest.sh env        print exports for libpq clients (eval "$(...)")
#   scripts/pgtest.sh conf       print the postgresql.conf lines that make sift loadable
#   scripts/pgtest.sh regress [t...]  make installcheck against the cluster; with test names, ONE pg_regress run for all of them in one fresh database
#                                (contrib_regression_<first>, the output in $SIFT_TMP/regress/<first>), then one leak check of the server log and one look at what the run left in the
#                                database (scripts/leftovers.sql, exceptions in scripts/leftovers.allow). A run that only differs names the tests that differ and runs each alone; a run
#                                with a leak or a leftover is cut in halves, each in a fresh database, until the failing tests are named, at most SIFT_BISECT_REDS (4) of them. A crashed
#                                backend (TRAP, PANIC, a signal) stops at once and names the test that crashed it; SIFT_REGRESS_NO_BISECT=1 never runs a test again: the findings of the batch, exit 1
#                                (SIFT_REGRESS_DIR: another directory of sql/ and expected/, for scripts/test-regress-batch.sh)
#                                Run the tests that compare heap-free reads (col_heapfree_*, col_track_*, col_tomb_*, col_stmt_cid*) on a quiet cluster: a session
#                                of another test holds back the xmin horizon, and what a read may take from the log without the heap (free_reads_heap) changes by design
#   scripts/pgtest.sh fresh [t...]    regress with a NEW backend for every statement outside a transaction (scripts/fresh-psql.py, test/fresh/README.md)
#   scripts/pgtest.sh leftovers [t...]  each test alone in a database of its own; lists what it left behind (scripts/leftovers.sql)
#   scripts/pgtest.sh tap [t]    TAP shell tests test/t/*.sh (or test/t/<t>.sh), own clusters (SIFT_TAP_KEEP=1 keeps the data directories of a failed test)
#   scripts/pgtest.sh ready      wait until the cluster accepts connections (recovery after a crashed backend)
#   scripts/pgtest.sh clean      drop the databases a job left in the cluster (all but postgres and the test database), once it is ready
#   scripts/pgtest.sh run CMD... CMD against a cluster of its own (SIFT_TEST_FORMAT, port SIFT_PORT + 1: the lane of scripts/ci.sh keeps SIFT_PORT), then the leak gate of stop
#   scripts/pgtest.sh stop       stop the cluster
#   scripts/pgtest.sh destroy    stop and remove $SIFT_TMP
#   scripts/pgtest.sh sanitizer-report  print ASan/UBSan findings (exit 1 if any)
#   scripts/pgtest.sh initdb-template DIR   the data directory SIFT_INITDB_TEMPLATE points at (1 MB WAL segments)
#   scripts/pgtest.sh new-data DIR [wal]    a fresh data directory (the template copied, else initdb); wal: the default 16 MB WAL
#                                           segments, for the tests whose point is WAL (always a real initdb)
#
# SIFT_TEST_FORMAT: f1 (default: the tests of format major 1 keep their indexes in it), f2 (SIFT_TESTING=1, sift.test_create_format1 = off: every CREATE INDEX and
# sift_table builds Format 2.0, what the high-level suites run on) or release (the release library, SIFT_TESTING= empty: no sift.test_* setting is given to the server).
#
# SIFT_TMP (default /tmp/sift-arch) and SIFT_PORT (56100-56299, default
# 56100) let several jobs run clusters side by side; SIFT_ROOT (default
# $SIFT_TMP/root) lets clusters in other SIFT_TMP directories share one install; TAP clusters use
# SIFT_PORT + 1 and up. SIFT_TESTING=1 (default) builds the sift._test_*
# functions; SIFT_TESTING= builds the release library.
#
# VULKAN=1 install builds the opt-in Vulkan backend (docs/GPU.md).
# SIFT_PREBUILT_ROOT: a root a build job staged (scripts/ci-build.sh, flavor in its .flavor): install compiles nothing, and before 18 copies it into the server's directories.
# SIFT_STAGE=1 stages the install under SIFT_ROOT on any major (the build job). SIFT_MAKE_JOBS: the compiler processes of install (default: the cores).
# SANITIZE=1 install builds with ASan+UBSan; SIFT_HARDEN=pkg|init|1 builds with the hardening flags of scripts/harden.mk
# (docs/SAFETY.md; SIFT_POINTER_OVERFLOW=1 adds the pointer-wrap trap); install runs make clean whenever SANITIZE, SIFT_TESTING or SIFT_HARDEN changes. start then preloads the sanitizer runtimes
# into the server, with LeakSanitizer (scripts/lsan.supp); reports go to
# $SIFT_TMP/sanitizer/ (sanitizer-report). SIFT_INITDB_TEMPLATE, a data directory made by `initdb-template`, is copied
# instead of running initdb for every cluster (scripts/ci.sh makes one per run, test/t/lib.bash uses it too).
# Every cluster is under the leak gate of scripts/leakcheck.sh: start records the memory of the
# long-lived processes, stop checks the cluster, stops it cleanly (fast, so that LeakSanitizer
# reports) and scans the logs. Exit 1 on a finding.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
# macOS: the postmaster dies with "became multithreaded" unless LC_ALL names a valid locale (the default C of a non-login shell does not count)
[[ $(uname) != Darwin ]] || export LC_ALL=en_US.UTF-8
TMP=${SIFT_TMP:-/tmp/sift-arch}
PORT=${SIFT_PORT:-56100}
DB=${SIFT_DB:-sift}
ROOT=${SIFT_PREBUILT_ROOT:-${SIFT_ROOT:-$TMP/root}}
export SIFT_ROOT=$ROOT
DATA=$TMP/data
SOCK=$TMP/sock
SANDIR=$TMP/sanitizer
SHAREDIR=$ROOT$(pg_config --sharedir)
PKGLIBDIR=$ROOT$(pg_config --pkglibdir)
. "$(dirname "$0")/leakcheck.sh"
PG_MAJOR=$(pg_config --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
# the cluster is this one and no other: a default of the caller (a service, a database) must not send a client to the system PostgreSQL
unset PGDATABASE PGSERVICE PGSERVICEFILE
export PGHOST=$SOCK PGPORT=$PORT
TEST_FORMAT=${SIFT_TEST_FORMAT:-f1}
case $TEST_FORMAT in
	f1|f2) ;;
	release) SIFT_TESTING= ;;
	*) echo "pgtest: SIFT_TEST_FORMAT must be f1, f2 or release, not '$TEST_FORMAT'" >&2; exit 1 ;;
esac
export SIFT_TESTING=${SIFT_TESTING-1}
FLAVOR="${SANITIZE:+asan }${SIFT_TESTING:+testing }${SIFT_HARDEN:+harden$SIFT_HARDEN${SIFT_UBSAN_SET:+(${SIFT_UBSAN_SET// /,})}${SIFT_POINTER_OVERFLOW:++ptr} }$([[ -n ${SIFT_LTO:-} ]] && echo lto || echo nolto)${VULKAN:+ vulkan} pg$PG_MAJOR"

(( PORT >= 56100 && PORT <= 56299 )) || { echo "SIFT_PORT must be in 56100-56299" >&2; exit 1; }

DLSUFFIX=.so; [[ $(uname) != Darwin ]] || DLSUFFIX=.dylib   # the suffix of a PostgreSQL module

# stage_prebuilt: SIFT_PREBUILT_ROOT is a root a build job staged (scripts/ci-build.sh): nothing is compiled here. Before 18 the files go into the server's own directories
stage_prebuilt() {
	[[ -f $ROOT/.flavor ]] || { echo "pgtest: SIFT_PREBUILT_ROOT=$ROOT holds no staged build (.flavor is missing)" >&2; return 1; }
	echo "pgtest: install: prebuilt root (flavor $(<"$ROOT/.flavor"))" >&2
	(( PG_MAJOR >= 18 )) || tar -C "$ROOT" --exclude=./.flavor --exclude=./.checks -cf - . | ${SIFT_SUDO:-} tar -C / --no-overwrite-dir -xf -
}

install() {
	[[ -z ${SIFT_PREBUILT_ROOT:-} ]] || { stage_prebuilt; return; }
	echo "pgtest: install: $([[ -n $SIFT_TESTING ]] && echo 'test library' || echo 'release library') (flavor ${FLAVOR% })" >&2   # SIFT_TESTING set and empty is the release library: an inherited empty value builds it
	[[ $(cat "$REPO/.build-flavor" 2>/dev/null) == "$FLAVOR" ]] || make -C "$REPO" USE_PGXS=1 clean >/dev/null
	# on the make command line: sudo resets the environment, which would install another flavor
	local flavor=(USE_PGXS=1 SIFT_TESTING="$SIFT_TESTING" SANITIZE="${SANITIZE:-}" SIFT_LTO="${SIFT_LTO:-}" SIFT_HARDEN="${SIFT_HARDEN:-}" ${SIFT_UBSAN_SET:+"SIFT_UBSAN_SET=$SIFT_UBSAN_SET"} ${SIFT_POINTER_OVERFLOW:+SIFT_POINTER_OVERFLOW=1} ${VULKAN:+VULKAN=1})
	nice make -C "$REPO" -j"${SIFT_MAKE_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}" "${flavor[@]}"
	"$REPO/scripts/check-undefined.sh" "$REPO/pg_sift$DLSUFFIX"
	echo "$FLAVOR" >"$REPO/.build-flavor"
	if (( PG_MAJOR >= 18 )) || [[ -n ${SIFT_STAGE:-} ]]; then
		make -C "$REPO" "${flavor[@]}" DESTDIR="$ROOT" install >/dev/null
	else
		${SIFT_SUDO:-} make -C "$REPO" "${flavor[@]}" install >/dev/null
		mkdir -p "$ROOT"
	fi
	echo "$FLAVOR" >"$ROOT/.flavor"
}

# The setting only the test library of this tree defines (the release library has none): a released library that runs on the data directory of the cluster (scripts/old-lib-check.sh) warns that
# "sift" is a reserved prefix for it. The cluster of `start` gets it on the command line of the server of this tree, not in its postgresql.conf (`conf common`).
# The clusters run with the default sift.on_corruption (quarantine), so that every write of every test goes through the guard of the quarantine; a test that asserts a corruption error says SET sift.on_corruption = error
# the build chooser (sift.col_build_source = auto) reads the index a REINDEX replaces and says so in a notice with the seconds it took: the clusters of the regression tests read the heap, as
# before 5.1, so that no expected output carries those seconds; the tests of the chooser say SET sift.col_build_source = auto
# CREATE INDEX builds format major 2: the tests keep the indexes they create in format major 1 (the testing build only); the tests of format major 2 say SET sift.test_create_format1 = off
CREATE_FORMAT1=on; [[ $TEST_FORMAT != f2 ]] || CREATE_FORMAT1=off
TREE_CONF="${SIFT_TESTING:+sift.col_build_source = heap$'\n'sift.test_scope_patch = on$'\n'sift.test_create_format1 = $CREATE_FORMAT1$'\n'sift.test_quiet_build_notice = on}"
# SIFT_MEMCENSUS=KB|on: every statement of every test that grows memory by KB (on: 1024) writes a SIFTMEM line to the server log
[[ -z ${SIFT_TESTING:-} || -z ${SIFT_MEMCENSUS:-} ]] || TREE_CONF+=$'\n'"sift.test_memctx_census = $([[ $SIFT_MEMCENSUS == on ]] && echo 1024 || echo "$SIFT_MEMCENSUS")"
tree_options() { local l; [[ -z $TREE_CONF ]] || while IFS= read -r l; do printf -- '-c %s ' "${l// = /=}"; done <<<"$TREE_CONF"; }

# The lines that make the staged sift loadable (the server's own install on PostgreSQL 17); `conf common`: those that any release of the library accepts
conf() {
	leak_conf
	# the background pass of a sift_table is on by default; a test counts the segments, blocks and dead rows its own statements leave, so the
	# clusters run without it as without autovacuum, and the tests of the pass switch it on
	# CREATE EXTENSION ends with a notice that the library is not preloaded (sql/5.0/24-install-notices.sql); no expected output carries it. Not a sift. setting: any release accepts it
	echo "pg_sift.install_notice = off"
	echo "sift.async_maintenance = off"
	# the tests of the scope index patch the members that changed whatever the patch costs; the test of the price says SET sift.test_scope_patch = off
	[[ ${1:-} == common || -z $TREE_CONF ]] || echo "$TREE_CONF"
	# a server with LLVM compiles every plan above jit_above_cost, and the tests that force a plan with page costs of 10000 reach it: the normal plan of 1,200
	# partitions took 5.6 s to compile and 11 ms to run (PostgreSQL 17 of the CI runners has the library in its package, 18 in another one)
	echo "jit = off"
	# 19 can aggregate below a join (eager aggregation) and prices that plan under some of those of the pushdown tests, which then keep the normal plan
	(( PG_MAJOR < 19 )) || echo "enable_eager_aggregate = off"
	(( PG_MAJOR >= 18 )) || return 0
	echo "extension_control_path = '$SHAREDIR:\$system'"
	echo "dynamic_library_path = '$PKGLIBDIR:\$libdir'"
}

# The server binary is not instrumented, so the ASan runtime must be preloaded.
sanitizer_env() { [[ $(cat "$ROOT/.flavor" 2>/dev/null) != asan* ]] || leak_sanitizer_env "$SANDIR"; }

# The cluster every test starts from: superuser postgres, UTF8, no locale. The clusters run with fsync off, so initdb need not sync
# either. WAL segments of 1 MB (the default 16 MB is a floor of 64 MB of pg_wal per cluster) except for KIND wal.
initdb_template() {
	local seg=(--wal-segsize=1)
	[[ ${2:-} != wal ]] || seg=()
	initdb --no-sync -D "$1" -A trust -U postgres -E UTF8 --no-locale --no-instructions "${seg[@]}" >/dev/null
}

# new_data DIR [wal]: a fresh data directory, copied from SIFT_INITDB_TEMPLATE when there is one
new_data() {
	if [[ -n ${SIFT_INITDB_TEMPLATE:-} && ${2:-} != wal ]]; then cp -a "$SIFT_INITDB_TEMPLATE" "$1"; else initdb_template "$1" "${2:-}"; fi
}

# No autovacuum in the cluster: a worker holds a snapshot while it analyzes a table, a catalog too, and VACUUM removes only the rows older
# than every snapshot of the database, so the rows a test deleted stay and its plan or its dead-document count differs now and then (the
# reloption autovacuum_enabled cannot switch off the workers' catalogs). The one job that needs it (scripts/soak.sh) turns it on for its run.
# max_wal_size 512MB: with 48MB a test that writes 30 MB of WAL in 14 s (col_sorted_run) checkpointed all the time, and on a runner disk that wrote
# 0.1 MB/s a backend waited 100 to 250 s for a dirty buffer (33 to 255 "checkpoints are occurring too frequently" per lane in a PostgreSQL 19 quick shard).
# The WAL a lane keeps is at most this, 1 MB segments, no recycling. A data directory in RAM (tmpfs) has no disk to wait for, and 8 lanes at 512MB
# held 2 GB of WAL at once: there it is 128MB.
wal_cap() { [[ $(df -P "$TMP" 2>/dev/null | awk 'NR == 2 { print $1 }') == tmpfs ]] && echo 128MB || echo 512MB; }
# reset_auto_conf DIR: the settings an ALTER SYSTEM of an earlier session left (max_parallel_workers_per_gather = 0, a small shared_buffers, work_mem) change the plans of
# the tests: 18 of them failed on a clean tree for it. The cluster starts from the conf above alone.
reset_auto_conf() {
	if grep -qv '^#' "$1/postgresql.auto.conf" 2>/dev/null; then
		echo "pgtest: $1/postgresql.auto.conf held settings from ALTER SYSTEM; emptied" >&2
		: >"$1/postgresql.auto.conf"
	fi
}

start() {
	mkdir -p "$SOCK"
	if [[ ! -f $DATA/PG_VERSION ]]; then
		new_data "$DATA"
		cat >>"$DATA/postgresql.conf" <<-EOF
			port = $PORT
			listen_addresses = ''
			unix_socket_directories = '$SOCK'
			$(conf common)
			fsync = off
			max_wal_size = $(wal_cap)
			min_wal_size = 80MB
			wal_recycle = off
			shared_buffers = 64MB
			max_worker_processes = 16
			max_locks_per_transaction = 256
			max_prepared_transactions = 8
			autovacuum = off
		EOF
	fi
	reset_auto_conf "$DATA"
	( sanitizer_env; pg_ctl -D "$DATA" -l "$TMP/postgres.log" -o "$(tree_options)" -w start >/dev/null )
	createdb -U postgres "$DB" 2>/dev/null || true
	leak_baseline "$DATA"
}

stop() {
	[[ -f $DATA/postmaster.pid ]] || return 0
	PGUSER=postgres leak_end "$DATA" "$TMP" pg_ctl -D "$DATA" -m fast -w stop >/dev/null
}

# run CMD...: start, CMD with the libpq environment of the cluster, stop; the leak gate of stop fails the run
run() {
	local rc=0
	export SIFT_PORT=$((PORT + 1))
	"$0" start || return 1
	( eval "$("$0" env)"; "$@" ) || rc=$?
	"$0" stop || rc=1
	return $rc
}

# ready: waits until the cluster accepts connections (SIFT_READY_SECS, 300). A backend that dies (a sanitizer that halts it, a PANIC test) makes the postmaster
# recover every other session; under ASan that takes a minute, and a job that starts in it fails at once with "the database system is not yet accepting connections"
ready() {
	local i
	for ((i = 0; i < ${SIFT_READY_SECS:-300} * 2; i++)); do
		pg_isready -q -h "$SOCK" -p "$PORT" && return 0
		[[ -f $DATA/postmaster.pid ]] || break
		sleep 0.5
	done
	echo "pgtest: the cluster at $SOCK does not accept connections (no postmaster, or not ready after ${SIFT_READY_SECS:-300} s)" >&2
	return 1
}

# clean: drops every database but the template ones, postgres and the test database: what a job created
clean() {
	local d
	ready || return 1
	for d in $(psql -U postgres -d postgres -X -qAt -c "SELECT datname FROM pg_database WHERE datname NOT IN ('postgres', 'template0', 'template1', '$DB')"); do
		dropdb -U postgres --force "$d"
	done
	psql -U postgres -d postgres -X -qAt -c CHECKPOINT >/dev/null   # the WAL of the job goes with its databases (no recycling: a checkpoint removes it; scripts/ci.sh counts the cluster in the scratch of the lane)
}

# regress [TEST...]: without names, make installcheck (the Makefile's database and test list). With names, ONE pg_regress run for all of them in one
# fresh database (contrib_regression_<first>, the output in $TMP/regress/<first>), then ONE look at the server log (leak_after) and ONE at what the
# run left in the database (scripts/leftovers.sql; scripts/leftovers.allow gives the objects a test may leave, and why). A run that finds a leak or a
# leftover is cut in halves, each in a fresh database, until the single tests that fail are named; a run that only differs runs the tests that differ alone (name_failures):
# the green path pays the checks once for the batch, and the failure path says which test is guilty, whatever an earlier test of the batch left behind. The first pass stays whole in $TMP/regress/<first> (its
# regression.diffs and report): a cut keeps its output in $TMP/regress/cut/ and drops its database at once, so that a batch of failures does not fill the scratch
# directory of the lane (the watchdog of scripts/ci/scratch.sh stops every server of the run when it passes its cap).
regress() {
	if (( ! $# )); then make -C "$REPO" USE_PGXS=1 installcheck PGUSER=postgres; return; fi
	batch_run "$@" && return 0
	if (( $# == 1 )); then batch_report "$1" "$1"; return 1; fi
	name_failures "$@"
	return 1
}

# What a backend that died leaves in the findings of leak_after: the server log of the batch (TRAP, PANIC, a signal) or a sanitizer that halted it
CRASH_RE='TRAP:|PANIC:|terminated by (signal|exception)|ERROR: AddressSanitizer|runtime error:'

# failed_tests KEY: the tests pg_regress marked "not ok" in the output of the batch KEY, in the order they ran (pg_regress removes regression.out when every test passed)
failed_tests() { [[ ! -f $TMP/regress/$1/regression.out ]] || sed -n 's/^not ok  *[0-9][0-9]*  *- *\([^ ]*\).*/\1/p' "$TMP/regress/$1/regression.out"; }

# name_failures TEST...: the batch of TEST... failed; says which tests are to blame, running the fewest more batches that can tell:
#  - a crashed backend: the first test that failed in the progress of pg_regress is the one that crashed it, and the tests after it fail for the recovery; nothing is run again
#  - SIFT_REGRESS_NO_BISECT set: nothing is run again, the findings are all there is
#  - only diffs (no leak, no leftover): the tests that differ run alone; one that fails alone is guilty. If all of them pass alone, an earlier test of the batch bleeds into them, and the cuts find it
#  - a leak or a leftover: the batch is cut in halves, because no test is named yet
name_failures() {
	local key=$1 report=$TMP/regress/$1.report failed first after rc=0
	failed=$(failed_tests "$key" | paste -sd' ' -) first=${failed%% *}
	after=${failed#"$first"}
	BISECT_NAMED=0
	if grep -qE "^leak in .*($CRASH_RE)" "$report"; then
		echo "pgtest: regress: a backend crashed in the batch of $# tests starting at $1; not cutting it. Crashed in: ${first:-no test failed in the progress, see the findings}; failed after it:${after:- none}" >&2
		grep -E "^leak in .*($CRASH_RE)" "$report" | head -n 5 >&2
		return
	fi
	if [[ -n ${SIFT_REGRESS_NO_BISECT:-} ]]; then
		echo "pgtest: regress: SIFT_REGRESS_NO_BISECT: not cutting the batch of $# tests starting at $1; failed in the batch: ${failed:-none named, see the findings}; what the batch found:" >&2
		cat "$report"
		return
	fi
	if [[ -n $failed ]] && ! grep -q '^not ok - ' "$report"; then
		rerun_alone $failed && return || rc=$?
		(( rc == 1 )) || return
		echo "pgtest: regress: $failed fail in the batch and pass alone; cutting the batch in halves to find the test they depend on" >&2
	else
		echo "pgtest: regress: the batch of $# tests starting at $1 failed; cutting it in halves to name the tests" >&2
	fi
	bisect_batch "$(<"$report")" "$@" || true
}

# rerun_alone TEST...: each TEST in a fresh database of its own. 0 when one of them fails alone (it is named), 1 when all pass alone, 2 when the server is gone
rerun_alone() {
	local t rc
	for t; do
		cut_batch "$t" && rc=0 || rc=$?
		(( rc != 2 )) || return 2
	done
	(( BISECT_NAMED > 0 )) || return 1
}

# batch_key TEST...: the name of the output of a batch in $TMP/regress: <first> for the whole batch, cut/<first>-<count> for a half or less of it (REGRESS_CUT is set)
batch_key() { echo "${REGRESS_CUT:+cut/}$1${REGRESS_CUT:+-$#}"; }

# batch_run TEST...: the batch in one pg_regress run and its checks; the findings (diffs, leaks, leftovers) go to $TMP/regress/<key>.report, not to the terminal
# (a cut, REGRESS_CUT=1, also keeps the progress of pg_regress back, and drops its database and the output of a batch that passed). 0 when every test passed and left nothing
batch_run() {
	local key db rc=0
	key=$(batch_key "$@")
	db=contrib_regression_$1${REGRESS_CUT:+_$#}
	batch_checks "$key" "$db" "$@" || rc=1
	[[ -z ${REGRESS_CUT:-} ]] || dropdb -U postgres --force --if-exists "$db" || rc=1
	(( rc )) || [[ -z ${REGRESS_CUT:-} ]] || rm -rf "$TMP/regress/$key" "$TMP/regress/$key.report"
	return $rc
}

batch_checks() {
	local key=$1 db=$2 out report pats now left rc=0 unit
	shift 2
	out=$TMP/regress/$key report=$TMP/regress/$key.report unit=regress-${key//\//-}
	mkdir -p "$(dirname "$out")"; : >"$report"
	leak_before "$unit" "$TMP/postgres.log"
	if ! if [[ -n ${REGRESS_CUT:-} ]]; then pg_regress "$out" "$db" "$@" >/dev/null; else pg_regress "$out" "$db" "$@"; fi; then
		rc=1; cat "$out/regression.diffs" >>"$report" 2>/dev/null
	fi
	leak_after "$unit" "$DATA" "$db" 2>>"$report" || { rc=1; echo "not ok - leaks memory or resources" >>"$report"; }
	(( rc == 0 )) || return 1
	pats=$(IFS='|'; echo "$*")
	now=$(db_left "$db")
	left=$(grep -vEf <(sed -n "s/^\($pats\): \(.*[^ ]\) *# .*/\2/p" "$REPO/scripts/leftovers.allow") <<<"$now" || true)
	[[ -z $left ]] && return 0
	{ sed "s/^/LEFTOVER: /" <<<"$left"; echo "not ok - leaves objects behind: end each test with the DROPs of what it creates (the extension last), or give the object and the reason in scripts/leftovers.allow"; } >>"$report"
	return 1
}

# batch_report KEY TEST: what the failed run of the single TEST (output KEY) found, named after it
batch_report() { sed "s/^LEFTOVER: /LEFTOVER $2: /; s/^not ok - /not ok - $2 /" "$TMP/regress/$1.report"; }

# the failing tests the cuts of one batch have named: a batch is red with 4 named as with 20, and the cuts of 20 reds ran past CI_JOB_TIMEOUT, so they stop at SIFT_BISECT_REDS (4)
BISECT_NAMED=0

# bisect_batch REPORT TEST...: REPORT is what the batch of TEST... found. Runs each half alone; 1 as soon as a half fails (the tests that fail alone are reported by name),
# else the tests only fail together: REPORT is all there is to say. 2 when the server is gone: no half can run
bisect_batch() {
	local report=$1 half rc1 rc2
	shift
	half=$(( $# / 2 ))
	cut_batch "${@:1:half}" && rc1=0 || rc1=$?
	(( rc1 != 2 )) || return 2
	cut_batch "${@:half+1}" && rc2=0 || rc2=$?
	(( rc2 != 2 )) || return 2
	(( rc1 || rc2 )) || { echo "pgtest: regress: no half of the batch fails alone, so it is the tests together; what the batch found:" >&2; echo "$report"; }
	return 1
}
cut_batch() {
	local REGRESS_CUT=1
	(( BISECT_NAMED < ${SIFT_BISECT_REDS:-4} )) || { echo "pgtest: regress: $BISECT_NAMED failing tests are named (SIFT_BISECT_REDS), not cutting further; not run alone: $*" >&2; return 1; }
	ready || { echo "pgtest: regress: the server is gone, so the batch of $# tests starting at $1 was not run" >&2; return 2; }
	batch_run "$@" && return 0
	if (( $# == 1 )); then BISECT_NAMED=$(( BISECT_NAMED + 1 )); batch_report "$(batch_key "$@")" "$1"; return 1; fi
	bisect_batch "$(<"$TMP/regress/$(batch_key "$@").report")" "$@"
}

# db_left DB: what scripts/leftovers.sql finds in DB (the objects of the database, not those of the cluster)
db_left() { psql -U postgres -d "$1" -X -qAt -v cluster=off -f "$REPO/scripts/leftovers.sql"; }

# pg_regress OUTDIR DB TEST...: pg_regress in a database it creates, the output in a fresh OUTDIR
pg_regress() {
	local out=$1 db=$2
	shift 2
	rm -rf "$out" && mkdir -p "$out"
	# SIFT_FRESH: a psql that connects anew before every statement outside a transaction (scripts/fresh-psql.py) stands first in PATH, in this subshell only
	# (FRESH_PSQL is the real psql, found before PATH changes: the shim refuses to run as its own psql or inside itself)
	(
		if [[ -n ${SIFT_FRESH:-} ]]; then
			mkdir -p "$TMP/fresh-bin" && ln -sf "$REPO/scripts/fresh-psql.py" "$TMP/fresh-bin/psql"
			FRESH_PSQL=$(command -v psql)
			export FRESH_PSQL FRESH_STATS=$out/fresh-stats PATH=$TMP/fresh-bin:$PATH
		fi
		PGUSER=postgres "$(dirname "$(pg_config --pgxs)")/../test/regress/pg_regress" --inputdir="${SIFT_REGRESS_DIR:-$REPO/test}" --outputdir="$out" --bindir= --dbname="$db" "$@"
	)
}

# leftovers [TEST...]: each test alone in a database of its own (all of test/sql without names); prints "TEST: object" for what it left behind
# (scripts/leftovers.sql: relations, functions, types, schemas, extensions, roles, ALTER SYSTEM...) and exits 1 when any test left something
leftovers() {
	local t db bad=0 before now left
	(( $# )) || set -- $(cd "$REPO/test/sql" && ls *.sql | sed 's/\.sql$//')
	cluster_left() { psql -U postgres -d postgres -X -qAt -v cluster=on -f "$REPO/scripts/leftovers.sql" | { grep -E '^(role|ALTER SYSTEM) ' || true; }; }
	before=$(cluster_left)
	for t in "$@"; do
		db=contrib_regression_$t
		pg_regress "$TMP/regress/$t" "$db" "$t" >/dev/null 2>&1 || echo "$t: FAILED, what it left may come from that (diffs in $TMP/regress/$t)"
		now=$(cluster_left)
		left=$(db_left "$db"; grep -vxFf <(echo "$before") <<<"$now" || true)
		[[ -z ${left//$'\n'/} ]] || { bad=1; echo "$left" >"$TMP/regress/$t/leftovers"; sed "s/^/$t: /" <<<"$left" | head -12; (( $(wc -l <<<"$left") <= 12 )) || echo "$t: ... all $(wc -l <<<"$left") in $TMP/regress/$t/leftovers"; }
		before=$now
		dropdb -U postgres --force "$db"
	done
	return $bad
}

tap() {
	local tests=(test/t/*.sh)
	[[ -n $1 ]] && tests=(test/t/$1.sh)
	cd "$REPO"
	# the sanitizer runtime goes into the servers (SIFT_PRELOAD) and not into prove, bash or psql
	( sanitizer_env; SIFT_PRELOAD=${LD_PRELOAD:-}; unset LD_PRELOAD; export SIFT_PRELOAD
	  SIFT_TMP=$TMP SIFT_ROOT=$ROOT SIFT_PORT=$PORT prove ${PROVE_FLAGS:---failures --comments} --exec "$REPO/scripts/budget.sh tap" "${tests[@]}" )
}

case ${1:-} in
	install) install ;;
	start) start ;;
	stop) stop ;;
	run) shift; run "$@" ;;
	conf) conf ;;
	psql) shift; exec psql -U postgres -d "$DB" -X "$@" ;;
	env)
		echo "export PGHOST=$SOCK PGPORT=$PORT PGUSER=postgres SIFT_ROOT=$ROOT"
		sanitizer_env
		[[ -z ${ASAN_OPTIONS:-} ]] || echo "export SIFT_SANITIZER='LD_PRELOAD=$LD_PRELOAD ASAN_OPTIONS=$ASAN_OPTIONS LSAN_OPTIONS=$LSAN_OPTIONS UBSAN_OPTIONS=$UBSAN_OPTIONS'"
		;;
	regress) shift; regress "$@" ;;
	fresh)
		shift
		# at most SIFT_FRESH_RUNS (default 1, at most 4) fresh runs on a host (every statement is a connection), and never inside one (the shim stands first in PATH of the pg_regress subshell only)
		[[ -z ${FRESH_PSQL:-}${FRESH_ACTIVE:-} ]] || { echo "pgtest: fresh runs do not nest" >&2; exit 1; }
		slot=0 got=
		for ((slot = 0; slot < ${SIFT_FRESH_RUNS:-1} && slot < 4; slot++)); do
			exec 9>"${XDG_RUNTIME_DIR:-/tmp}/sift-fresh.$slot.lock"
			if flock -n 9; then got=1; break; fi
		done
		[[ -n $got ]] || { echo "pgtest: ${SIFT_FRESH_RUNS:-1} fresh run(s) are at work (SIFT_FRESH_RUNS, at most 4)" >&2; exit 1; }
		SIFT_FRESH=1 regress "$@" ;;
	leftovers) shift; leftovers "$@" ;;
	reset-auto-conf) reset_auto_conf "$2" ;;
	initdb-template) initdb_template "$2" ;;
	new-data) new_data "$2" "${3:-}" ;;
	tap) tap "${2:-}" ;;
	ready) ready ;;
	clean) clean ;;
	destroy) stop || true; rm -rf "$TMP" ;;
	sanitizer-report) leak_scan "$SANDIR" ;;
	*) sed -n "2,32p" "$0"; exit 1 ;;
esac
