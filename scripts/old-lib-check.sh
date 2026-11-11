#!/usr/bin/env bash
# The refusal path of the table write path: a sift_table that has had an unfrozen group (rows with their own versions, column format 1.4, the
# metapage on-disk format 1.3) must not be read by an older library, which would show rows of running or aborted writers as final. The older
# library has to refuse with a format error and return no row.
#
#   scripts/old-lib-check.sh build                      build the older library (git archive of SIFT_OLD_REF, installed under a root of its
#                                                       own, cached); prints the root
#   scripts/old-lib-check.sh check DATADIR PORT SOCKDIR the checks against the data directory of a stopped cluster (tables ctl, unf, unf_v,
#                                                       unf_f of `run`) with a server that loads the older library
#   scripts/old-lib-check.sh run                        the whole of it: a cluster of this tree writes the tables, then `check`
#   scripts/old-lib-check.sh points                     point directories: the older library (SIFT_OLD_REF, default v3.6.0) writes an index with them, this tree reads the same
#                                                       files as it reads its own (answers, verify, writes, a merge that writes column format 33), and the older
#                                                       library refuses the merged index with the format error, and reads it again after a REINDEX at the setting 14
#   scripts/old-lib-check.sh gorder                      the same for the global orders (the addresses of a chunk with the bits of its largest offset; column format 38)
#   scripts/old-lib-check.sh keyed                      the same for the units of a key-cut index (the chunk table after the directory; column format 34)
#   scripts/old-lib-check.sh include                    INCLUDE columns (segment format 5, metapage minor 1.5): the older library (SIFT_OLD_REF, default v3.7.0) writes an index without them, this tree
#                                                       reads it as it reads its own and writes beside it; an index of this tree that names INCLUDE columns is refused by the older library
#                                                       with the format error and reported by sift.upgrade_check('3.7') as a blocker, and an index without them is read again after
#                                                       DROP INDEX and CREATE INDEX
#   scripts/old-lib-check.sh track                      the logs of 4.0.0 to 4.1.1 (SIFT_OLD_REF, default v4.1.0): the older library counts row versions that statements with a moved command id left without a tombstone (three
#                                                       tables: the first UPDATE of a session, a volatile function, a second index), this tree checks the heap for those logs and answers as the heap, and
#                                                       re-baselines each of them, by VACUUM, by the first read of an idle table and by the maintenance worker of a write, with no other step; the older library
#                                                       then reads the repaired logs as the heap
#   scripts/old-lib-check.sh present                    the presence leaf of a jsonb column that may hold a NULL document (4.2; SIFT_OLD_REF, default v4.1.4): this tree writes one index with the leaf and one over a
#                                                       NOT NULL column without it; the older library refuses the first with the layout error and reads the second, REINDEX under the older library
#                                                       writes the layout it reads, and this tree then reads that index as it reads its own (answers, verify) and writes the leaf again at the next REINDEX
#   scripts/old-lib-check.sh scope                      the scope index of 5.0 (column format minor 45; SIFT_OLD_REF, default v4.3.0): this tree writes one index with a scope and one without; the older library refuses the first
#                                                       with the format error and the standard hint (use a newer pg_sift), reads the second and the heap, and reads a plain index made in its place of the scope index
#   scripts/old-lib-check.sh twohop                     the same for a scope of two paths (column format minor 49): the older library refuses it, an index with a scope of one path stays at 45
#   scripts/old-lib-check.sh upgrade                    the other way: the older library writes a table with the groups it tags under pressure (a
#                                                       committed load and a rolled back one), this tree reads them as it reads its own and VACUUM
#                                                       freezes and drops them
#
# Prints PASS / FAIL / SKIP lines (test/t/old_lib_refusal.sh turns them into TAP); exit 0 when all pass, 1 on a FAIL, 77 when there is nothing
# to run (no older release in the repository, an older release than 3.2.0 on a CPU other than x86-64, PostgreSQL 17, which has no extension_control_path to hold two builds, PostgreSQL 19, which no release builds on, or no testing build).
#
# SIFT_OLD_REF (default v3.0.1; the library of the tree with the 3.1 write path is the one under test), SIFT_OLD_SRC (a source tree of
# the older release instead of the archive), SIFT_OLD_DIR (where it is built, default ~/.cache/sift/old-lib/<commit>), SIFT_TMP (scratch, default under /dev/shm) and SIFT_PORT as
# for scripts/pgtest.sh (`run`), ROWS (rows of a table, default 40000), KEEP=1 (keep the cluster of `run`).
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
# the column format a feature introduced, from the constant of the tree (src/col/api/format.h): no minor is written in this script, so that a feature that raises one cannot leave it behind
colfmt() { sed -n "s/^#define SIFT_COL_FORMAT_$1[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p" "$REPO/src/col/api/format.h"; }
PG="$REPO/scripts/pgtest.sh"
OLD_REF=${SIFT_OLD_REF:-v3.0.1}
ROWS=${ROWS:-40000}
FORMAT_ERROR='on-disk format 1\.3|column format 1\.4'
FAILED=0

pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILED=1; }
skip() { echo "SKIP $*" >&2; exit 77; }

build() {
	local rev dir major
	major=$(pg_config --version | sed -n 's/^PostgreSQL \([0-9][0-9]*\).*/\1/p')
	((major == 18)) || skip "PostgreSQL $major: 17 cannot hold two builds (no extension_control_path) and the released libraries do not build on 19"
	# their CPU probes are x86 only: 3.2.0 is the first release that builds elsewhere
	[[ $(uname -m) == x86_64 || $(printf '%s\n' "${OLD_REF#v}" 3.2.0 | sort -V | head -1) == 3.2.0 ]] || skip "$OLD_REF builds on x86-64 only (AArch64 since 3.2.0)"
	if [[ -n ${SIFT_OLD_SRC:-} ]]; then
		rev=$(git -C "$SIFT_OLD_SRC" rev-parse --short HEAD 2>/dev/null || echo src)
	else
		rev=$(git -C "$REPO" rev-parse --verify -q --short "$OLD_REF^{commit}") || skip "no $OLD_REF in this repository"
	fi
	dir=${SIFT_OLD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/sift/old-lib/$rev}
	if [[ ! -f $dir/root/.flavor ]]; then
		mkdir -p "$dir/src"
		# the tests that need the same older release start together on two lanes: the second waits for the build of the first
		exec 9>"$dir.lock"
		! command -v flock >/dev/null || flock 9
		if [[ ! -f $dir/root/.flavor ]]; then
			if [[ -n ${SIFT_OLD_SRC:-} ]]; then cp -a "$SIFT_OLD_SRC/." "$dir/src/"; else git -C "$REPO" archive "$rev" | tar -x -C "$dir/src"; fi
			# the release flavor, the library users get (not the ASan one of a sanitized run: the older server does not preload the runtime); its own
			# scratch and port (the port is only checked, the cluster is not started)
			# CPATH and LIBRARY_PATH carry the directories of pg_config's flags to a release whose Makefile does not (lz4 and zstd of Homebrew): else it is built
			# without them and cannot read the chunks this tree writes
			( cd "$dir/src" && CPATH=$(pg_config --cppflags | tr ' ' '\n' | sed -n 's/^-I//p' | paste -sd: -) LIBRARY_PATH=$(pg_config --ldflags | tr ' ' '\n' | sed -n 's/^-L//p' | paste -sd: -) \
				SIFT_TMP=$dir SIFT_ROOT=$dir/root SIFT_PORT=56100 SIFT_TESTING= SANITIZE= scripts/pgtest.sh install >"$dir/build.log" 2>&1 ) ||
				{ tail -20 "$dir/build.log" >&2; echo "FAIL building $OLD_REF" >&2; exit 1; }
		fi
		exec 9>&-
	fi
	echo "$dir/root"
}

# psql against the older server: out and err in the globals OUT and ERR, the exit status in RC
OLD_PSQL=()
query() {
	local errfile
	errfile=$(mktemp)
	OUT=$("${OLD_PSQL[@]}" -c "$1" 2>"$errfile")
	RC=$?
	ERR=$(<"$errfile")
	rm -f "$errfile"
}
expect_refused() { # DESCRIPTION SQL
	query "$2"
	if ((RC != 0)) && [[ -z $OUT && $ERR =~ $FORMAT_ERROR ]]; then pass "$1"; else fail "$1 (rc $RC, rows [$OUT], error [$ERR])"; fi
}
expect_rows() { # DESCRIPTION SQL WANT
	query "$2"
	if ((RC == 0)) && [[ $OUT == "$3" ]]; then pass "$1"; else fail "$1 (rc $RC, got [$OUT], want [$3], error [$ERR])"; fi
}

# old_start ROOT DATA PORT SOCKDIR LOG: the server on the files of DATA with the library of the older release in ROOT; OLD_PSQL is its client. 1 and the end of its log when it does not start
old_start() {
	local root=$1 data=$2 port=$3 sock=$4 log=$5 share lib opts
	share=$root$(pg_config --sharedir)
	lib=$root$(pg_config --pkglibdir)
	opts="-c port=$port -c listen_addresses='' -c unix_socket_directories='$sock' -c 'extension_control_path=$share:\$system' -c 'dynamic_library_path=$lib:\$libdir'"
	pg_ctl -D "$data" -l "$log" -o "$opts" -w -t 120 start >/dev/null 2>&1 || { tail -20 "$log" >&2; return 1; }
	OLD_PSQL=(psql -X -At -q -h "$sock" -p "$port" -U postgres -d postgres -v ON_ERROR_STOP=1)
}

check() { # DATADIR PORT SOCKDIR
	local data=$1 port=$2 sock=$3 root log
	root=$(build) || exit $?
	log=$(dirname "$data")/old-lib.log
	old_start "$root" "$data" "$port" "$sock" "$log" || { fail "the server with the older library starts"; return; }
	checks "$log"
	pg_ctl -D "$data" -m fast -w stop >/dev/null 2>&1 || pg_ctl -D "$data" -m immediate stop >/dev/null 2>&1
}

checks() { # LOG of the older server
	local log=$1 want="$ROWS:$((ROWS * (ROWS + 1) / 2))"

	query "SELECT sift.version()"
	if [[ $OUT == 3.0* ]]; then pass "the server runs the older library ($OUT)"; else fail "the server runs the older library (got [$OUT], error [$ERR])"; fi
	expect_rows "a table that never had an unfrozen group is read as before" "SELECT count(*) || ':' || sum(id) FROM ctl" "1000:500500"
	expect_refused "a table with unfrozen groups is refused by a scan" "SELECT count(*) FROM unf"
	expect_refused "... by a fetch of one row" "SELECT * FROM unf LIMIT 1"
	expect_refused "... by an INSERT" "INSERT INTO unf VALUES (0, 'x')"
	expect_refused "... by VACUUM" "VACUUM unf"
	expect_refused "a table loaded by COPY into column groups is refused by a scan" "SELECT count(*) FROM unf_b"
	expect_refused "a table whose unfrozen groups were frozen is refused as well (the format is never lowered)" "SELECT count(*) FROM unf_v"
	expect_rows "a table rewritten by VACUUM FULL is read again" "SELECT count(*) || ':' || sum(id) FROM unf_f" "$want"
	query "SELECT 1"
	if [[ $OUT == 1 ]]; then pass "the older server is still up"; else fail "the older server is still up (got [$OUT], error [$ERR])"; fi
	if grep -qE 'TRAP|terminated by signal|PANIC' "$log"; then fail "no crash in the log of the older server"; else pass "no crash in the log of the older server"; fi
}

# a table written by a loop of single-row INSERTs in one transaction, with the directory of 16 entries: it has unfrozen groups
load_table() { # NAME
	"$PG" psql -X -qAt -v ON_ERROR_STOP=1 -d postgres -c "SET sift.table_max_segments = 16" -c "SET sift.table_write_buffer_limit = 2048" \
		-c "CREATE TABLE $1 (id int, v text) USING sift_table WITH (autovacuum_enabled = false)" -c "BEGIN" \
		-c "DO \$\$ BEGIN FOR i IN 1..$ROWS LOOP INSERT INTO $1 VALUES (i, repeat('x', 1000) || i); END LOOP; END \$\$" -c "COMMIT" >/dev/null
}

run() {
	local data
	export SIFT_TMP=${SIFT_TMP:-$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/sift-old-lib-run}
	export SIFT_PORT=${SIFT_PORT:-56140}
	build >/dev/null || exit $?
	[[ -f ${SIFT_ROOT:-$SIFT_TMP/root}/.flavor ]] || "$PG" install >/dev/null 2>&1 || { echo "FAIL building the tree under test" >&2; exit 1; }
	[[ -n ${KEEP:-} ]] || trap '"$PG" destroy >/dev/null 2>&1' EXIT
	"$PG" start >/dev/null || { echo "FAIL starting the cluster" >&2; exit 1; }
	[[ $("$PG" psql -X -qAt -d postgres -c "SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_sift_test'") == 1 ]] ||
		skip "the build under test is not the testing build (sift.table_max_segments)"
	"$PG" psql -X -qAt -d postgres -c "CREATE EXTENSION pg_sift" -c "CREATE EXTENSION pg_sift_test" >/dev/null
	# sift.col_format_minor = 3: the files a 3.0.x library reads (the default minor 4 writes the type tags it does not know)
	"$PG" psql -X -qAt -d postgres -c "SET sift.col_format_minor = 3" -c "CREATE TABLE ctl (id int, v text) USING sift_table" -c "INSERT INTO ctl SELECT g, 'c' || g FROM generate_series(1, 1000) g" \
		-c "VACUUM ctl" >/dev/null
	load_table unf
	if [[ $("$PG" psql -X -qAt -d postgres -c "SELECT sift._test_tbl_tagged('unf') > 0") == t ]]; then pass "the loop left a table with unfrozen groups"
	else fail "the loop left a table with unfrozen groups"; echo "tables not written by the new write path: nothing to refuse"; return 1; fi
	# a table loaded by COPY into column groups carries the same unfrozen groups
	"$PG" psql -X -qAt -v ON_ERROR_STOP=1 -d postgres -c "SET sift.table_bulk_groups = on" -c "CREATE TABLE unf_b (id int, v text) USING sift_table WITH (autovacuum_enabled = false)" \
		-c "COPY unf_b FROM PROGRAM 'seq 1 2000 | sed ''s/.*/&\trow &/'''" >/dev/null
	if [[ $("$PG" psql -X -qAt -d postgres -c "SELECT sift._test_tbl_tagged('unf_b') > 0") == t ]]; then pass "a COPY into column groups left a table with unfrozen groups"
	else fail "a COPY into column groups left a table with unfrozen groups"; fi
	load_table unf_v
	load_table unf_f
	"$PG" psql -X -qAt -d postgres -c "VACUUM unf_v" -c "SET sift.col_format_minor = 3" -c "VACUUM (FULL) unf_f" >/dev/null
	"$PG" stop >/dev/null || { fail "the cluster stops cleanly"; return 1; }
	data=$SIFT_TMP/data
	# the cluster is stopped: the socket directory and the port are free for the older server
	check "$data" "$SIFT_PORT" "$SIFT_TMP/sock"
}

# the older library under pressure (the release build has no lowered directory: 3.0.1 tags a write buffer once the directory of 128 entries is half
# full, so the load is about 130 MB in write buffers of 2 MB), then this tree on the same files
upgrade() {
	local root log rows=${UP_ROWS:-150000} want
	want="$rows:$((rows * (rows + 1) / 2))"
	export SIFT_TMP=${SIFT_TMP:-$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/sift-old-lib-upgrade}
	export SIFT_PORT=${SIFT_PORT:-56140}
	root=$(build) || exit $?
	[[ -f ${SIFT_ROOT:-$SIFT_TMP/root}/.flavor ]] || "$PG" install >/dev/null 2>&1 || { echo "FAIL building the tree under test" >&2; exit 1; }
	[[ -n ${KEEP:-} ]] || trap '"$PG" destroy >/dev/null 2>&1' EXIT
	"$PG" start >/dev/null && "$PG" stop >/dev/null || { echo "FAIL starting the cluster" >&2; exit 1; }
	log=$SIFT_TMP/old-lib-upgrade.log
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts"; return; }
	"${OLD_PSQL[@]}" -c "CREATE EXTENSION pg_sift" -c "SET sift.table_write_buffer_limit = 2048" -c "CREATE TABLE up_c (id int, v text) USING sift_table WITH (autovacuum_enabled = false)" \
		-c "CREATE TABLE up_a (id int, v text) USING sift_table WITH (autovacuum_enabled = false)" -c "BEGIN" \
		-c "INSERT INTO up_c SELECT g, repeat('x', 1000) || g FROM generate_series(1, $rows) g" -c "COMMIT" -c "BEGIN" \
		-c "INSERT INTO up_a SELECT g, repeat('x', 1000) || g FROM generate_series(1, $rows) g" -c "ROLLBACK" >/dev/null ||
		{ fail "the older library writes the tables"; pg_ctl -D "$SIFT_TMP/data" -m immediate stop >/dev/null 2>&1; return; }
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1 || { fail "the older server stops cleanly"; return; }
	"$PG" start >/dev/null || { fail "the cluster of this tree starts on the files of the older library"; return; }
	"$PG" psql -X -qAt -d postgres -c "CREATE EXTENSION IF NOT EXISTS pg_sift_test" >/dev/null 2>&1
	psql_new() { "$PG" psql -X -qAt -d postgres "$@" 2>&1; }
	OUT=$(psql_new -c "SELECT sift._test_tbl_tagged('up_c') > 0 AND sift._test_tbl_tagged('up_a') > 0")
	[[ $OUT == t ]] && pass "the older library left groups that carry their writer" || fail "the older library left groups that carry their writer (got [$OUT])"
	[[ $(psql_new -c "SELECT count(*) || ':' || sum(id) FROM up_c") == "$want" ]] && pass "the committed load is read" || fail "the committed load is read"
	[[ $(psql_new -c "SELECT count(*) FROM up_a") == 0 ]] && pass "the rolled back load is not" || fail "the rolled back load is not"
	[[ $(psql_new -c "SET sift.enable_agg = off" -c "SELECT count(*) || ':' || sum(id) FROM up_c" | tail -1) == "$want" ]] && pass "... by a plain scan too" || fail "... by a plain scan too"
	psql_new -c "INSERT INTO up_c VALUES (0, 'new')" >/dev/null
	[[ $(psql_new -c "SELECT count(*) FROM up_c") == "$((rows + 1))" ]] && pass "the table takes new rows" || fail "the table takes new rows"
	psql_new -c "VACUUM up_c" -c "VACUUM up_a" >/dev/null
	[[ $(psql_new -c "SELECT sift._test_tbl_tagged('up_c') + sift._test_tbl_tagged('up_a')") == 0 ]] && pass "VACUUM freezes the committed groups and drops the rolled back ones" ||
		fail "VACUUM freezes the committed groups and drops the rolled back ones"
	[[ $(psql_new -c "SELECT count(*) || ':' || sum(id) FROM up_c" -c "SELECT count(*) FROM up_a" | tr '\n' ' ') == "$((rows + 1)):$((rows * (rows + 1) / 2)) 0 " ]] && pass "the rows after VACUUM" || fail "the rows after VACUUM"
	"$PG" stop >/dev/null
}

# What the older library (SIFT_OLD_REF, default v3.6.0) writes, then this tree on the same files: the answers of $CY_SQL (with the settings CY_FAST, against the heap's CY_HEAP), verify, writes
# and a promotion that put a segment of column format $CY_NEW beside the older ones, a merge that writes only it, the refusal of the older library, and its reading after a REINDEX at the setting 14.
# CY_NAME names the cluster, CY_SETUP (an array of statements) makes the table and the index $CY_IDX of the older library, which writes column format $CY_OLDMIN or more and below $CY_NEW;
# CY_WHAT says what is read; CY_INSERT is the statement that adds rows
cycle() {
	local root log new_md5 old_md5 s
	OLD_REF=${SIFT_OLD_REF:-v3.6.0}
	export SIFT_TMP=${SIFT_TMP:-$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/sift-old-lib-$CY_NAME}
	export SIFT_PORT=${SIFT_PORT:-56140}
	root=$(build) || exit $?
	[[ -f ${SIFT_ROOT:-$SIFT_TMP/root}/.flavor ]] || "$PG" install >/dev/null 2>&1 || { echo "FAIL building the tree under test" >&2; exit 1; }
	[[ -n ${KEEP:-} ]] || trap '"$PG" destroy >/dev/null 2>&1' EXIT
	"$PG" start >/dev/null && "$PG" stop >/dev/null || { echo "FAIL starting the cluster" >&2; exit 1; }
	log=$SIFT_TMP/old-lib-$CY_NAME.log
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts"; return; }
	for s in "CREATE EXTENSION pg_sift" "${CY_SETUP[@]}"; do
		"${OLD_PSQL[@]}" -c "$s" >/dev/null || { fail "the older library writes the index"; pg_ctl -D "$SIFT_TMP/data" -m immediate stop >/dev/null 2>&1; return; }
	done
	old_md5=$("${OLD_PSQL[@]}" "${CY_FAST[@]/#/-c}" -c "$CY_SQL" | tail -1)
	[[ $("${OLD_PSQL[@]}" -c "SELECT min(format) >= $CY_OLDMIN AND max(format) < $CY_NEW FROM sift.col_segments('$CY_IDX') WHERE kind = 'immutable'") == t ]] &&
		pass "the older library wrote column format $CY_OLDMIN or more, below $CY_NEW" || fail "the older library wrote column format $CY_OLDMIN or more, below $CY_NEW"
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1 || { fail "the older server stops cleanly"; return; }
	"$PG" start >/dev/null || { fail "the cluster of this tree starts on the files of the older library"; return; }
	psql_new() { "$PG" psql -X -qAt -d postgres "$@" 2>&1; }
	new_md5=$(psql_new "${CY_FAST[@]/#/-c}" -c "$CY_SQL" | tail -1)
	[[ $new_md5 == "$old_md5" ]] && pass "this tree reads the $CY_WHAT of the older library with its answers" || fail "this tree reads the $CY_WHAT of the older library (got [$new_md5], want [$old_md5])"
	[[ $(psql_new "${CY_HEAP[@]/#/-c}" -c "$CY_SQL" | tail -1) == "$old_md5" ]] && pass "... and with the heap's" || fail "... and with the heap's"
	[[ $(psql_new -c "SELECT count(*) FROM (SELECT sift.col_verify('$CY_IDX', true)) z") == 1 ]] && pass "sift.col_verify finds the tables sound" || fail "sift.col_verify finds the tables sound"
	psql_new -c "$CY_INSERT" -c "SELECT sift.col_promote('$CY_IDX'::regclass)" >/dev/null
	[[ $(psql_new -c "SELECT min(format) < $CY_NEW AND max(format) >= $CY_NEW FROM sift.col_segments('$CY_IDX') WHERE kind = 'immutable'") == t ]] &&
		pass "a new segment is column format $CY_NEW or more beside the older one" || fail "a new segment is column format $CY_NEW or more beside the older one"
	new_md5=$(psql_new "${CY_FAST[@]/#/-c}" -c "$CY_SQL" | tail -1)
	old_md5=$(psql_new "${CY_HEAP[@]/#/-c}" -c "$CY_SQL" | tail -1)
	[[ $new_md5 == "$old_md5" ]] && pass "both layouts answer as the heap" || fail "both layouts answer as the heap (got [$new_md5], want [$old_md5])"
	psql_new -c "SELECT sift.col_merge('$CY_IDX'::regclass, true)" >/dev/null
	[[ $(psql_new -c "SELECT min(format) = max(format) AND max(format) >= $CY_NEW FROM sift.col_segments('$CY_IDX') WHERE kind = 'immutable'") == t ]] && pass "a merge writes column format $CY_NEW or more, one for all its segments" || fail "a merge writes column format $CY_NEW or more, one for all its segments"
	# the writer stamps the newest minor of what a segment holds, which may be above the one this cycle is about (a merge adds what a promotion does not)
	NEWF=$(psql_new -c "SELECT max(format) FROM sift.col_segments('$CY_IDX') WHERE kind = 'immutable'")
	[[ $(psql_new -c "SELECT count(*) FROM (SELECT sift.col_verify('$CY_IDX', true)) z") == 1 ]] && pass "sift.col_verify finds the merged tables sound" || fail "sift.col_verify finds the merged tables sound"
	new_md5=$(psql_new "${CY_FAST[@]/#/-c}" -c "$CY_SQL" | tail -1)
	[[ $new_md5 == "$old_md5" ]] && pass "the merged index answers as the heap" || fail "the merged index answers as the heap"
	"$PG" stop >/dev/null || { fail "the cluster of this tree stops cleanly"; return; }
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts again"; return; }
	FORMAT_ERROR="column format 1\\.$NEWF"
	expect_refused "the older library refuses column format $CY_NEW or more with the format error" "$CY_REFUSED"
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1
	"$PG" start >/dev/null || { fail "the cluster of this tree starts again"; return; }
	psql_new -c "SET sift.col_format_minor = 14" -c "REINDEX INDEX $CY_IDX" >/dev/null
	"$PG" stop >/dev/null
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts after the REINDEX"; return; }
	expect_rows "the older library reads the index again after REINDEX at the setting 14, with the answers of the heap" "$(printf '%s; ' "${CY_FAST[@]}")$CY_SQL" "$old_md5"
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1
}

# the point directories of the older library (a power of two of slots)
points() {
	CY_NAME=points CY_IDX=pk_i CY_OLDMIN=16 CY_NEW=$(colfmt EXACTSLOT) CY_WHAT="point directories"
	local w="k = 777 OR k IN (1, 2, 3, 99999, 100002) OR s = 's5' OR s IN ('s1', 'none') OR id = 4242"	# a key present, absent, a list, two columns, and a text key
	CY_SQL="SELECT md5(string_agg(id::text, ',' ORDER BY id)) || ':' || count(*) FROM pk WHERE $w"
	CY_SETUP=("CREATE TABLE pk AS SELECT g AS id, (g * 7919) % 100003 AS k, 's' || (g * 31) % 6000 AS s FROM generate_series(1, 60000) g"
		"CREATE INDEX pk_i ON pk USING sift_col (id, k, s) WITH (key_cut = 'off', point_columns = 'id', point_tid_columns = 'k, s')")
	CY_FAST=("SET enable_seqscan = off" "SET enable_bitmapscan = off")
	CY_HEAP=("SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET sift.enable_col_scan = off")
	CY_INSERT="INSERT INTO pk SELECT g, (g * 7919) % 100003, 's' || (g * 31) % 6000 FROM generate_series(60001, 90000) g"
	CY_REFUSED="SET enable_seqscan = off; SELECT count(*) FROM pk WHERE k = 777"
	cycle
}

# the units of a key-cut index of the older library (the chunk table after the directory)
keyed() {
	CY_NAME=keyed CY_IDX=kk_i CY_OLDMIN=17 CY_NEW=$(colfmt TABFIRST) CY_WHAT="key-cut units"
	CY_SQL="SELECT md5(string_agg(tenant::text || ':' || n || ':' || s, ',' ORDER BY tenant)) FROM (SELECT tenant, count(*) n, sum(amount) s FROM kk WHERE amount < 700 OR tenant = 7 GROUP BY tenant) x"
	CY_SETUP=("CREATE TABLE kk AS SELECT g AS id, g % 60 AS tenant, (g * 31) % 1000 AS amount FROM generate_series(1, 90000) g"
		"CREATE INDEX kk_i ON kk USING sift_col (id, tenant, amount) WITH (key_cut = 'tenant')")
	CY_FAST=("SET sift.agg_preagg = off" "SET sift.agg_force = on")
	CY_HEAP=("SET sift.enable_agg = off")
	CY_INSERT="ALTER INDEX kk_i SET (point_tid_columns = 'off'); INSERT INTO kk SELECT g, g % 60, (g * 31) % 1000 FROM generate_series(90001, 130000) g"
	CY_REFUSED="SET sift.agg_force = on; SELECT count(*) FROM kk WHERE tenant = 7"
	cycle
}

# the global orders of the older library (u32 addresses of 9 bits) beside the packed addresses of this tree (column format 38; an integer point directory, no dictionary searched in place, which is 39)
gorder() {
	CY_NAME=gorder CY_IDX=go_i CY_OLDMIN=30 CY_NEW=$(colfmt ADDRPACK) CY_WHAT="global orders"
	local w="(ts >= timestamp '2025-02-01' AND ts < timestamp '2025-02-03') OR k = 777 OR k IN (1, 2, 99999, 100002)"
	CY_SQL="SELECT md5(string_agg(id::text, ',' ORDER BY id)) || ':' || count(*) FROM go WHERE $w"
	CY_SETUP=("CREATE TABLE go AS SELECT g AS id, (g * 7919) % 100003 AS k, timestamp '2025-01-01' + ((g::bigint * 104729) % 5000000) * interval '1 second' AS ts FROM generate_series(1, 60000) g"
		"CREATE INDEX go_i ON go USING sift_col (id, k, ts) WITH (key_cut = 'off', point_tid_columns = 'k', order_columns = 'ts', order_array_columns = 'ts')")
	CY_FAST=("SET enable_seqscan = off" "SET enable_bitmapscan = off")
	CY_HEAP=("SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET sift.enable_col_scan = off")
	CY_INSERT="INSERT INTO go SELECT g, (g * 7919) % 100003, timestamp '2025-01-01' + ((g::bigint * 104729) % 5000000) * interval '1 second' FROM generate_series(60001, 90000) g"
	CY_REFUSED="SET enable_seqscan = off; SELECT count(*) FROM go WHERE k = 777"
	cycle
}

# the plain index of the older library, read by this tree; an index with INCLUDE columns of this tree, refused by the older library
include() {
	local root log want ids
	OLD_REF=${SIFT_OLD_REF:-v3.7.0}
	export SIFT_TMP=${SIFT_TMP:-$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/sift-old-lib-include}
	export SIFT_PORT=${SIFT_PORT:-56142}
	root=$(build) || exit $?
	[[ -f ${SIFT_ROOT:-$SIFT_TMP/root}/.flavor ]] || "$PG" install >/dev/null 2>&1 || { echo "FAIL building the tree under test" >&2; exit 1; }
	[[ -n ${KEEP:-} ]] || trap '"$PG" destroy >/dev/null 2>&1' EXIT
	"$PG" start >/dev/null && "$PG" stop >/dev/null || { echo "FAIL starting the cluster" >&2; exit 1; }
	log=$SIFT_TMP/old-lib-include.log
	ids="SELECT md5(string_agg(id::text, ',' ORDER BY id)) || ':' || count(*) FROM"
	want="SELECT id FROM %s WHERE body @@> 'dim_a3 AND w7' AND category_id < 60 ORDER BY sift.score(ctid) DESC LIMIT 100000"
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts"; return; }
	"${OLD_PSQL[@]}" -c "CREATE EXTENSION pg_sift" \
		-c "CREATE TABLE inc AS SELECT g AS id, 'common dim_a' || g % 7 || ' w' || g % 30 AS body, g % 97 AS category_id, DATE '2026-01-01' + g % 300 AS created_at FROM generate_series(1, 30000) g" \
		-c "CREATE TABLE inc2 AS SELECT * FROM inc" -c "CREATE INDEX inc_i ON inc USING sift (body)" >/dev/null || { fail "the older library writes the index"; pg_ctl -D "$SIFT_TMP/data" -m immediate stop >/dev/null 2>&1; return; }
	OLD_MD5=$("${OLD_PSQL[@]}" -c "$ids ($(printf "$want" inc)) x")
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1 || { fail "the older server stops cleanly"; return; }
	"$PG" start >/dev/null || { fail "the cluster of this tree starts on the files of the older library"; return; }
	psql_new() { "$PG" psql -X -qAt -d postgres "$@" 2>&1; }
	[[ $(psql_new -c "$ids ($(printf "$want" inc)) x") == "$OLD_MD5" ]] && pass "this tree reads the index of the older library with its answers" || fail "this tree reads the index of the older library with its answers"
	# these two check the segment formats the older library reads: the write buffers they open are format 0 (a buffer of format 6, 4.2, makes the index 1.6, checked with w6_i below)
	PGOPTIONS='-c sift.test_wbuf_narrow=on' psql_new -c "INSERT INTO inc SELECT g, 'common dim_a' || g % 7 || ' w' || g % 30, g % 97, DATE '2026-01-01' + g % 300 FROM generate_series(30001, 40000) g" -c "SELECT sift.promote('inc_i')" >/dev/null
	PGOPTIONS='-c sift.test_wbuf_narrow=on' psql_new -c "CREATE INDEX inc2_i ON inc2 USING sift (body) INCLUDE (category_id, created_at)" >/dev/null
	psql_new -c "CREATE TABLE w6 AS SELECT g AS id, 'common dim_a' || g % 7 AS body FROM generate_series(1, 2000) g" -c "CREATE INDEX w6_i ON w6 USING sift (body)" -c "INSERT INTO w6 SELECT g, 'common w6' FROM generate_series(2001, 2100) g" >/dev/null
	uc=$(PGHOST=$SIFT_TMP/sock PGPORT=$SIFT_PORT PGUSER=postgres "$REPO/scripts/upgrade-check.sh" --to 3.7 postgres 2>&1)
	grep -qE '^blocker +inc2_i +index +format 1\.5' <<<"$uc" && pass "scripts/upgrade-check.sh --to 3.7 reports the index with INCLUDE columns (format 1.5) as a blocker" || fail "scripts/upgrade-check.sh --to 3.7 reports the index with INCLUDE columns as a blocker ($uc)"
	grep -qE '^ok +inc_i +index +format 1\.4' <<<"$uc" && pass "... and the index without them, written to by this tree, as ok (format 1.4)" || fail "... and the index without them, written to by this tree, as ok ($uc)"
	grep -qE '^blocker +w6_i +index +format 1\.6' <<<"$uc" && pass "... and an index that took an insert, whose write buffer is format 6 (1.6), as a blocker" || fail "... and an index that took an insert, whose write buffer is format 6 (1.6), as a blocker ($uc)"
	[[ $(psql_new -c "$ids ($(printf "$want" inc2)) x") == "$OLD_MD5" ]] && pass "the index with INCLUDE columns answers as the older library's" || fail "the index with INCLUDE columns answers as the older library's"
	"$PG" stop >/dev/null || { fail "the cluster of this tree stops cleanly"; return; }
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts again"; return; }
	FORMAT_ERROR='on-disk format 1\.5'
	expect_refused "the older library refuses an index with INCLUDE columns with the format error" "SET enable_seqscan = off; SELECT count(*) FROM inc2 WHERE body @@> 'dim_a3'"
	expect_rows "... and reads the index it wrote, with the rows of this tree, as the heap" "$ids (SELECT id FROM inc WHERE id <= 30000 AND body ~ 'dim_a3' AND body ~ 'w7' AND category_id < 60) x" "$OLD_MD5"
	FORMAT_ERROR='on-disk format 1\.6'
	expect_refused "the older library refuses an index whose write buffer is format 6 with the format error" "SET enable_seqscan = off; SELECT count(*) FROM w6 WHERE body @@> 'dim_a3'"
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1
	"$PG" start >/dev/null || { fail "the cluster of this tree starts again"; return; }
	psql_new -c "DROP INDEX inc2_i" -c "CREATE INDEX inc2_i ON inc2 USING sift (body)" >/dev/null
	"$PG" stop >/dev/null
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts after the new index"; return; }
	expect_rows "the older library reads the index without INCLUDE columns that DROP INDEX and CREATE INDEX wrote" "$ids ($(printf "$want" inc2)) x" "$OLD_MD5"
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1
}

# the scope index (column format 45; a scope of two paths: 49): the older library must refuse it, and read what has no scope
scope() {
	local root log want mode=${1:-scope} fmt decl setup
	OLD_REF=${SIFT_OLD_REF:-v4.3.0}
	export SIFT_TMP=${SIFT_TMP:-$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/sift-old-lib-$mode}
	export SIFT_PORT=${SIFT_PORT:-56146}
	if [[ $mode == twohop ]]; then
		fmt=$(colfmt TWOHOP)
		decl='tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)'
		setup=(-c "CREATE TABLE grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int)" -c "CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id)"
			-c "CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id)" -c "CREATE TABLE memberships (group_id int, dim_a_id int, PRIMARY KEY (group_id, dim_a_id))"
			-c "INSERT INTO grants SELECT m, 1 + m % 9, m, NULL FROM generate_series(1, 200) m" -c "INSERT INTO grants SELECT 200 + g, 1 + g % 9, NULL, g FROM generate_series(1, 20) g"
			-c "INSERT INTO memberships SELECT 1 + m % 20, m FROM generate_series(1, 210) m")
	else
		fmt=$(colfmt SCOPE)
		decl='tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)'
		setup=(-c "CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id))"
			-c "INSERT INTO tenant_scope SELECT m, 1 + (m * 7 + j) % 9 FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4"
			-c "CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')")
	fi
	root=$(build) || exit $?
	[[ -f ${SIFT_ROOT:-$SIFT_TMP/root}/.flavor ]] || "$PG" install >/dev/null 2>&1 || { echo "FAIL building the tree under test" >&2; exit 1; }
	[[ -n ${KEEP:-} ]] || trap '"$PG" destroy >/dev/null 2>&1' EXIT
	log=$SIFT_TMP/old-lib-scope.log
	psql_new() { "$PG" psql -X -qAt -d postgres "$@" 2>&1; }
	local q="SELECT count(*) || ':' || sum(v) FROM events" qp="SELECT count(*) || ':' || sum(v) FROM events_plain"
	"$PG" start >/dev/null || { echo "FAIL starting the cluster" >&2; exit 1; }
	psql_new -c "CREATE EXTENSION pg_sift" "${setup[@]}" \
		-c "CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int)" \
		-c "INSERT INTO events SELECT i, 1 + i % 210, timestamptz '2026-01-01' + i * interval '1 minute', i % 10 FROM generate_series(1, 20000) i" \
		-c "CREATE TABLE events_plain AS SELECT * FROM events" \
		-c "CREATE INDEX scope_ix ON events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = '$decl')" \
		-c "CREATE INDEX plain_ix ON events_plain USING sift_col (id, dim_a_id, created_at, v)" -c "VACUUM (FREEZE, ANALYZE) events, events_plain" >/dev/null ||
		{ fail "this tree writes the indexes"; "$PG" stop >/dev/null; return; }
	want=$(psql_new -c "SET sift.enable_agg = off" -c "$q" | tail -1)
	[[ $(psql_new -c "SELECT max(format) FROM sift.col_segments('scope_ix') WHERE kind = 'immutable'") == "$fmt" && \
		$(psql_new -c "SELECT max(format) < $fmt FROM sift.col_segments('plain_ix') WHERE kind = 'immutable'") == t ]] &&
		pass "this tree writes column format $fmt for the index with a scope and an older one for the index without" ||
		fail "this tree writes column format $fmt for the index with a scope and an older one for the index without"
	# the older library has not the functions of the event triggers of 5.0 and would fail every CREATE INDEX and DROP INDEX (docs/UPGRADING.md, `trigger`)
	psql_new -c "ALTER EVENT TRIGGER sift_col_build_lock DISABLE" -c "ALTER EVENT TRIGGER sift_col_value_store_sql_drop DISABLE" >/dev/null
	"$PG" stop >/dev/null || { fail "the cluster of this tree stops cleanly"; return; }
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts"; return; }
	FORMAT_ERROR="column format 1\\.$fmt"
	expect_refused "the older library refuses the index with a scope with the format error" "SET sift.agg_force = on; SET sift.enable_agg = on; SELECT count(*) FROM events WHERE v = 3"
	[[ $ERR == *'Use a newer pg_sift'* ]] && pass "... and the standard hint" || fail "... and the standard hint (got [$ERR])"
	expect_refused "... as it does a plain read of the table: the planner opens the indexes of the table" "SET sift.enable_agg = off; $q"
	expect_rows "... and reads the index without a scope, with the answers of the heap" "SET sift.agg_force = on; $qp" "$want"
	query "DROP INDEX scope_ix"
	((RC == 0)) && pass "the older library drops the index with a scope" || fail "the older library drops the index with a scope ($ERR)"
	expect_rows "... and reads the table as this tree does" "SET sift.enable_agg = off; $q" "$want"
	"${OLD_PSQL[@]}" -c "CREATE INDEX scope_ix ON events USING sift_col (id, dim_a_id, created_at, v)" >/dev/null && pass "... and makes an index without a scope in its place" || fail "... and makes an index without a scope in its place"
	expect_rows "that index answers as the heap" "SET sift.agg_force = on; $q" "$want"
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1
	if grep -qE 'TRAP|terminated by signal|PANIC' "$log"; then fail "no crash in the log of the older server"; else pass "no crash in the log of the older server"; fi
}

# 4.0.0 to 4.1.1 left row versions without a tombstone when the command id moved inside an UPDATE; the repair is the library's alone (docs/releases/v4.1.2.md)
track() {
	local root log t
	OLD_REF=${SIFT_OLD_REF:-v4.1.0}
	export SIFT_TMP=${SIFT_TMP:-$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/sift-old-lib-track}
	export SIFT_PORT=${SIFT_PORT:-56143}
	root=$(build) || exit $?
	[[ -f ${SIFT_ROOT:-$SIFT_TMP/root}/.flavor ]] || "$PG" install >/dev/null 2>&1 || { echo "FAIL building the tree under test" >&2; exit 1; }
	[[ -n ${KEEP:-} ]] || trap '"$PG" destroy >/dev/null 2>&1' EXIT
	"$PG" start >/dev/null && "$PG" stop >/dev/null || { echo "FAIL starting the cluster" >&2; exit 1; }
	log=$SIFT_TMP/old-lib-track.log
	# the answer of table $1 by the sift scan (on) or by the heap (off)
	q() { echo "SET sift.enable_agg = $2; SELECT string_agg(tenant || ':' || n || ':' || s, ' ' ORDER BY tenant) FROM (SELECT tenant, count(*) n, sum(amount) s FROM $1 GROUP BY tenant) q"; }
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts"; return; }
	for t in ta tb tc; do
		"${OLD_PSQL[@]}" -c "CREATE EXTENSION IF NOT EXISTS pg_sift" -c "CREATE TABLE $t (code text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 100)" \
			-c "INSERT INTO $t SELECT 'c' || i, i % 10, i FROM generate_series(1, 20000) i" -c "CREATE INDEX ${t}_c ON $t USING sift_col (code, tenant, amount)$([[ $t == tc ]] && echo ' WITH (write_buffer_limit = 64)')" \
			-c "VACUUM (FREEZE, ANALYZE) $t" >/dev/null || { fail "the older library writes the index of $t"; pg_ctl -D "$SIFT_TMP/data" -m immediate stop >/dev/null 2>&1; return; }
	done
	"${OLD_PSQL[@]}" -c "CREATE FUNCTION one() RETURNS int LANGUAGE plpgsql AS \$\$ BEGIN PERFORM 1; RETURN 0; END \$\$" >/dev/null
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1 && old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the older server restarts"; return; }
	# a new server: the first statement of the session is the UPDATE (the attach of the shared tables inside it), then a function that runs SQL, and a table that is written many times
	"${OLD_PSQL[@]}" -c "BEGIN" -c "UPDATE ta SET amount = amount + 1 WHERE code IN (SELECT 'c' || i FROM generate_series(7, 20000, 7) i)" \
		-c "UPDATE ta SET amount = amount + 5 WHERE code IN (SELECT 'c' || i FROM generate_series(5, 20000, 5) i)" -c "COMMIT" >/dev/null
	"${OLD_PSQL[@]}" -c "BEGIN" -c "UPDATE tb SET amount = amount + 1 + one() WHERE code IN (SELECT 'c' || i FROM generate_series(7, 20000, 7) i)" \
		-c "UPDATE tb SET amount = amount + 5 + one() WHERE code IN (SELECT 'c' || i FROM generate_series(5, 20000, 5) i)" -c "COMMIT" >/dev/null
	"${OLD_PSQL[@]}" -c "BEGIN" -c "UPDATE tc SET amount = amount + 1 + one() WHERE code IN (SELECT 'c' || i FROM generate_series(7, 20000, 7) i)" -c "COMMIT" >/dev/null
	for t in ta tb tc; do
		if [[ $("${OLD_PSQL[@]}" -c "$(q $t on)" | tail -1) != $("${OLD_PSQL[@]}" -c "$(q $t off)" | tail -1) ]]; then pass "the older library counts row versions of $t that were updated (the repro)"
		else fail "the older library counts row versions of $t that were updated (the repro)"; fi
	done
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1 || { fail "the older server stops cleanly"; return; }
	"$PG" start >/dev/null || { fail "the cluster of this tree starts on the files of the older library"; return; }
	psql_new() { "$PG" psql -X -qAt -d postgres "$@" 2>&1; }
	psql_new -c "CREATE EXTENSION IF NOT EXISTS pg_sift_test" >/dev/null
	hf() { psql_new -c "SELECT heap_free FROM sift.col_tracking('$1')" | tail -1; }
	right() { [[ $(psql_new -c "$(q $1 on)" | tail -1) == $(psql_new -c "$(q $1 off)" | tail -1) ]]; }
	# only the heap-free read goes wrong: the checked read (sift.test_no_heapfree) and the heap are the reference
	for t in ta tb tc; do
		right $t && [[ $(hf ${t}_c) == f ]] && pass "$t: this tree takes the log of the older library for incomplete, checks the heap and answers as the heap" ||
			fail "$t: this tree takes the log of the older library for incomplete, checks the heap and answers as the heap"
	done
	[[ $(psql_new -c "SELECT cur_flags FROM sift._test_log_chan('ta_c')") == 1 ]] && pass "the log of the older library has the flags 1" || fail "the log of the older library has the flags 1"
	# 1. the next VACUUM (sift.col_auto_merge is off in this cluster)
	psql_new -c "VACUUM tb" >/dev/null
	[[ $(hf tb_c) == t ]] && right tb && [[ $(psql_new -c "SELECT cur_flags FROM sift._test_log_chan('tb_c')") == 3 ]] && pass "tb: the next VACUUM re-baselines the log (flags 3), and the heap-free read is the heap's" || fail "tb: the next VACUUM re-baselines the log (flags 3), and the heap-free read is the heap's"
	# 2. a read of a table nobody writes or vacuums, with the worker on
	psql_new -c "ALTER SYSTEM SET sift.col_auto_merge = on" -c "SELECT pg_reload_conf()" >/dev/null
	psql_new -c "$(q ta on)" >/dev/null
	for _ in $(seq 1 60); do [[ $(hf ta_c) == t ]] && break; sleep 0.5; done
	[[ $(hf ta_c) == t ]] && right ta && pass "ta: the first read of an idle table queues the re-baseline, and the heap-free read is the heap's" || fail "ta: the first read of an idle table queues the re-baseline, and the heap-free read is the heap's"
	# 3. the maintenance worker that the writes of a table ask for
	# the requests of an index are one naptime (2 s) apart, and the older library made the last one a moment ago
	sleep 3
	psql_new -c "INSERT INTO tc SELECT 'n' || i, i % 10, i FROM generate_series(1, 30000) i" >/dev/null
	for _ in $(seq 1 60); do [[ $(psql_new -c "SELECT cur_flags FROM sift._test_log_chan('tc_c')") == 3 ]] && break; sleep 0.5; done
	[[ $(psql_new -c "SELECT cur_flags FROM sift._test_log_chan('tc_c')") == 3 ]] && right tc && pass "tc: the writes of a table ask for the worker, which re-baselines the log, and the heap-free read is the heap's" ||
		fail "tc: the writes of a table ask for the worker, which re-baselines the log, and the heap-free read is the heap's"
	"$PG" stop >/dev/null || { fail "the cluster of this tree stops cleanly"; return; }
	# and back: a log with the flags 3 is incomplete to the older library, which checks the heap
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts again"; return; }
	for t in ta tb; do
		[[ $("${OLD_PSQL[@]}" -c "$(q $t on)" | tail -1) == $("${OLD_PSQL[@]}" -c "$(q $t off)" | tail -1) ]] && pass "the older library reads the repaired $t as the heap (the flags 3 are an incomplete log to it)" ||
			fail "the older library reads the repaired $t as the heap (the flags 3 are an incomplete log to it)"
	done
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1
}

# the presence leaf of 4.2: refused by the older library, REINDEX under it as the way back
present() {
	local root log
	OLD_REF=${SIFT_OLD_REF:-v4.1.4}
	export SIFT_TMP=${SIFT_TMP:-$([[ -d /dev/shm ]] && echo /dev/shm || echo "${TMPDIR:-/tmp}")/sift-old-lib-present}
	export SIFT_PORT=${SIFT_PORT:-56144}
	root=$(build) || exit $?
	[[ -f ${SIFT_ROOT:-$SIFT_TMP/root}/.flavor ]] || "$PG" install >/dev/null 2>&1 || { echo "FAIL building the tree under test" >&2; exit 1; }
	[[ -n ${KEEP:-} ]] || trap '"$PG" destroy >/dev/null 2>&1' EXIT
	log=$SIFT_TMP/old-lib-present.log
	psql_new() { "$PG" psql -X -qAt -d postgres "$@" 2>&1; }
	local heap="SELECT count(*) || ':' || coalesce(sum(id), 0) FROM pn WHERE NOT (doc @> '{\"b\": \"x\"}')"
	"$PG" start >/dev/null || { echo "FAIL starting the cluster" >&2; exit 1; }
	local minor="SELECT 1"; [[ $OLD_REF == v3.* ]] && minor="SET sift.col_format_minor = 6"	# 3.2 reads column minors up to 6
	psql_new -c "$minor" -c "CREATE EXTENSION pg_sift" -c "CREATE TABLE pn (id int, g int, doc jsonb)" -c "CREATE TABLE pk (id int, g int, doc jsonb NOT NULL)" \
		-c "INSERT INTO pn SELECT i, i % 5, CASE i % 4 WHEN 0 THEN NULL WHEN 1 THEN '{\"b\": \"x\"}' ELSE '{\"b\": \"y\"}' END FROM generate_series(1, 8000) i" \
		-c "INSERT INTO pk SELECT i, i % 5, '{\"b\": \"x\"}' FROM generate_series(1, 8000) i" \
		-c "CREATE INDEX pn_c ON pn USING sift_col (id, g, doc) WITH (paths = 'doc.b astext')" -c "CREATE INDEX pk_c ON pk USING sift_col (id, g, doc) WITH (paths = 'doc.b astext')" \
		-c "VACUUM (FREEZE, ANALYZE) pn, pk" >/dev/null || { fail "this tree writes the indexes"; "$PG" stop >/dev/null; return; }
	local want leaves
	want=$(psql_new -c "SET sift.enable_agg = off" -c "$heap" | tail -1)
	[[ $(psql_new -c "SELECT (SELECT count(*) FROM sift.col_leaves('pn_c') WHERE path LIKE '%#present')::text || (SELECT count(*) FROM sift.col_leaves('pk_c') WHERE path LIKE '%#present')::text") == 10 ]] &&
		pass "this tree writes the leaf for the nullable column and none for the NOT NULL column" || fail "this tree writes the leaf for the nullable column and none for the NOT NULL column"
	"$PG" stop >/dev/null || { fail "the cluster of this tree stops cleanly"; return; }
	old_start "$root" "$SIFT_TMP/data" "$SIFT_PORT" "$SIFT_TMP/sock" "$log" || { fail "the server with the older library starts"; return; }
	FORMAT_ERROR='column layout'
	expect_refused "the older library refuses the index with the presence leaf with the layout error" "SELECT sift.col_verify('pn_c', true)"
	query "SELECT sift.col_verify('pk_c', true)"
	((RC == 0)) && pass "the older library reads the index of the NOT NULL column" || fail "the older library reads the index of the NOT NULL column ($ERR)"
	"${OLD_PSQL[@]}" -c "REINDEX INDEX pn_c" >/dev/null && pass "REINDEX under the older library writes the layout it reads" || fail "REINDEX under the older library writes the layout it reads"
	query "SELECT sift.col_verify('pn_c', true)"
	((RC == 0)) && pass "the older library verifies the rewritten index" || fail "the older library verifies the rewritten index ($ERR)"
	expect_rows "... and the index answers as the heap" "SET sift.enable_agg = on; $heap" "$want"
	"${OLD_PSQL[@]}" -c "INSERT INTO pn SELECT 8000 + i, i % 5, '{\"b\": \"x\"}' FROM generate_series(1, 500) i" -c "SELECT sift.col_promote('pn_c')" >/dev/null &&
		pass "the older library writes and promotes beside it" || fail "the older library writes and promotes beside it"
	want=$("${OLD_PSQL[@]}" -c "SET sift.enable_agg = off" -c "$heap" | tail -1)
	pg_ctl -D "$SIFT_TMP/data" -m fast -w stop >/dev/null 2>&1 || { fail "the older server stops cleanly"; return; }
	"$PG" start >/dev/null || { fail "the cluster of this tree starts on the files of the older library"; return; }
	verified() { [[ $(psql_new -c "SELECT sift.col_verify('pn_c', true)::text = ''") == t ]]; }
	verified && pass "this tree verifies the index the older library wrote" || fail "this tree verifies the index the older library wrote"
	[[ $(psql_new -c "SELECT count(*) FROM sift.col_leaves('pn_c') WHERE path LIKE '%#present'") == 0 ]] && pass "... which has no leaf" || fail "... which has no leaf"
	[[ $(psql_new -c "SET sift.enable_agg = on" -c "$heap" | tail -1) == "$want" ]] && pass "... and answers as the heap" || fail "... and answers as the heap"
	psql_new -c "INSERT INTO pn SELECT 9000 + i, i % 5, NULL FROM generate_series(1, 300) i" -c "SELECT sift.col_promote('pn_c')" >/dev/null &&
		verified && pass "this tree writes and promotes beside it, and the index verifies" || fail "this tree writes and promotes beside it, and the index verifies"
	want=$(psql_new -c "SET sift.enable_agg = off" -c "$heap" | tail -1)
	[[ $(psql_new -c "SET sift.enable_agg = on" -c "$heap" | tail -1) == "$want" ]] && pass "both layouts answer as the heap" || fail "both layouts answer as the heap"
	psql_new -c "REINDEX INDEX pn_c" >/dev/null
	[[ $(psql_new -c "SELECT count(*) FROM sift.col_leaves('pn_c') WHERE path LIKE '%#present'") == 1 ]] && pass "REINDEX with this tree writes the leaf again" || fail "REINDEX with this tree writes the leaf again"
	"$PG" stop >/dev/null || fail "the cluster of this tree stops cleanly"
}

case ${1:-} in
	build) build ;;
	check) shift; check "$@"; exit $FAILED ;;
	run) run; exit $FAILED ;;
	upgrade) upgrade; exit $FAILED ;;
	points) points; exit $FAILED ;;
	keyed) keyed; exit $FAILED ;;
	gorder) gorder; exit $FAILED ;;
	include) include; exit $FAILED ;;
	track) track; exit $FAILED ;;
	present) present; exit $FAILED ;;
	scope) scope; exit $FAILED ;;
	twohop) scope twohop; exit $FAILED ;;
	*) sed -n '2,12p' "$0"; exit 2 ;;
esac
