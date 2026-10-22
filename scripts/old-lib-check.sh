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
#   scripts/old-lib-check.sh upgrade                    the other way: the older library writes a table with the groups it tags under pressure (a
#                                                       committed load and a rolled back one), this tree reads them as it reads its own and VACUUM
#                                                       freezes and drops them
#
# Prints PASS / FAIL / SKIP lines (test/t/old_lib_refusal.sh turns them into TAP); exit 0 when all pass, 1 on a FAIL, 77 when there is nothing
# to run (no older release in the repository, PostgreSQL 17, which has no extension_control_path to hold two builds, PostgreSQL 19, which no release builds on, or no testing build).
#
# SIFT_OLD_REF (default v3.0.1; the library of the tree with the 3.1 write path is the one under test), SIFT_OLD_SRC (a source tree of
# the older release instead of the archive), SIFT_OLD_DIR (where it is built, default ~/.cache/sift/old-lib/<commit>), SIFT_TMP and SIFT_PORT as
# for scripts/pgtest.sh (`run`), ROWS (rows of a table, default 40000), KEEP=1 (keep the cluster of `run`).
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
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
		flock 9
		if [[ ! -f $dir/root/.flavor ]]; then
			if [[ -n ${SIFT_OLD_SRC:-} ]]; then cp -a "$SIFT_OLD_SRC/." "$dir/src/"; else git -C "$REPO" archive "$rev" | tar -x -C "$dir/src"; fi
			# the production flavor, the library users get (not the ASan one of a sanitized run: the older server does not preload the runtime); its own
			# scratch and port (the port is only checked, the cluster is not started)
			( cd "$dir/src" && SIFT_TMP=$dir SIFT_ROOT=$dir/root SIFT_PORT=56100 SIFT_TESTING= SANITIZE= scripts/pgtest.sh install >"$dir/build.log" 2>&1 ) ||
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

check() { # DATADIR PORT SOCKDIR
	local data=$1 port=$2 sock=$3 root share lib log opts
	root=$(build) || exit $?
	share=$root$(pg_config --sharedir)
	lib=$root$(pg_config --pkglibdir)
	log=$(dirname "$data")/old-lib.log
	opts="-c port=$port -c listen_addresses='' -c unix_socket_directories='$sock' -c 'extension_control_path=$share:\$system' -c 'dynamic_library_path=$lib:\$libdir'"
	pg_ctl -D "$data" -l "$log" -o "$opts" -w -t 120 start >/dev/null 2>&1 || { tail -20 "$log" >&2; fail "the server with the older library starts"; return; }
	OLD_PSQL=(psql -X -At -q -h "$sock" -p "$port" -U postgres -d postgres -v ON_ERROR_STOP=1)
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
	export SIFT_TMP=${SIFT_TMP:-${XDG_CACHE_HOME:-$HOME/.cache}/sift/old-lib-run}
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

# the older library under pressure (the production build has no lowered directory: 3.0.1 tags a write buffer once the directory of 128 entries is half
# full, so the load is about 130 MB in write buffers of 2 MB), then this tree on the same files
upgrade() {
	local root share lib log opts rows=${UP_ROWS:-150000} want
	want="$rows:$((rows * (rows + 1) / 2))"
	export SIFT_TMP=${SIFT_TMP:-${XDG_CACHE_HOME:-$HOME/.cache}/sift/old-lib-upgrade}
	export SIFT_PORT=${SIFT_PORT:-56140}
	root=$(build) || exit $?
	[[ -f ${SIFT_ROOT:-$SIFT_TMP/root}/.flavor ]] || "$PG" install >/dev/null 2>&1 || { echo "FAIL building the tree under test" >&2; exit 1; }
	[[ -n ${KEEP:-} ]] || trap '"$PG" destroy >/dev/null 2>&1' EXIT
	"$PG" start >/dev/null && "$PG" stop >/dev/null || { echo "FAIL starting the cluster" >&2; exit 1; }
	share=$root$(pg_config --sharedir)
	lib=$root$(pg_config --pkglibdir)
	log=$SIFT_TMP/old-lib-upgrade.log
	opts="-c port=$SIFT_PORT -c listen_addresses='' -c unix_socket_directories='$SIFT_TMP/sock' -c 'extension_control_path=$share:\$system' -c 'dynamic_library_path=$lib:\$libdir'"
	pg_ctl -D "$SIFT_TMP/data" -l "$log" -o "$opts" -w -t 120 start >/dev/null 2>&1 || { tail -20 "$log" >&2; fail "the server with the older library starts"; return; }
	OLD_PSQL=(psql -X -At -q -h "$SIFT_TMP/sock" -p "$SIFT_PORT" -U postgres -d postgres -v ON_ERROR_STOP=1)
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

case ${1:-} in
	build) build ;;
	check) shift; check "$@"; exit $FAILED ;;
	run) run; exit $FAILED ;;
	upgrade) upgrade; exit $FAILED ;;
	*) sed -n '2,12p' "$0"; exit 2 ;;
esac
