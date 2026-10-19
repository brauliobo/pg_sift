#!/usr/bin/env bash
# The release gate (docs/RELEASE.md): the risk-based set of jobs a release head must pass, run once per tree.
#
#   scripts/ci.sh release [--list] [--force] [--keep] [--only REGEX]
#
#   --list       print the key of the tree and the jobs, in order, and stop (nothing is built or run)
#   --force      run even when the tree already has a stamp
#   --keep       keep the scratch directory of every job (default: only the logs)
#   --only REGEX run just the jobs whose name matches (no stamp is written)
#
# The tree key is the hash of the committed src/ (all of it), test/, scripts/, Makefile, pg_sift.control and the pg_sift--*.sql scripts: a
# head whose key has a stamp ($RELEASE_STATE, default ~/.local/state/pg_sift-release/KEY) is not gated again; a commit that touches only
# docs, bench or packaging keeps the key. The jobs run one at a time, in a fixed order, on a copy of HEAD (git archive), inside a scope of
# 2 cores and 12 GB (CPUQuota=200%), at most 100000 rows. A job that fails is run once more alone (a CI job: only its failed parts): it
# passes the gate only when it passes then and every failed part matches scripts/release.flakes (regex, tab, cause); a second failure
# stops the run. After each job the server logs it left are searched for a server crash.
#
# Environment: RELEASE_BM25_BASE (default 544ddf0, the main the BM25 gate compares with), RELEASE_BM25_ACCEPT (cells allowed above 1.02),
# RELEASE_COL_REF (the gated ref the column gate compares with; default the head of the newest stamp), RELEASE_COL_MAX (1.15; between RELEASE_COL_NOTE, 1.02, and it a sift_col ratio is printed as accepted),
# RELEASE_OLD (the release the upgrade starts from, v2.0.3), RELEASE_MINOR2 (the last head that wrote column format 2, 46008df),
# RELEASE_MARIADB_SERIES (11.8), RELEASE_TMP (scratch root, /tmp), SIFT_PORT (first port, default 56130).
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
LIST='' FORCE='' KEEP='' ONLY=.
while (( $# )); do
	case $1 in
		--list) LIST=1 ;; --force) FORCE=1 ;; --keep) KEEP=1 ;; --only) ONLY=$2; shift ;;
		*) sed -n '2,8p' "$0"; exit 1 ;;
	esac
	shift
done
STATE=${RELEASE_STATE:-${XDG_STATE_HOME:-$HOME/.local/state}/pg_sift-release}
BM25_BASE=${RELEASE_BM25_BASE:-544ddf0} BM25_ACCEPT=${RELEASE_BM25_ACCEPT:-session|vacuum_nofree|idle1_rank}
COL_MAX=${RELEASE_COL_MAX:-1.15} COL_NOTE=${RELEASE_COL_NOTE:-1.02} OLD=${RELEASE_OLD:-v2.0.3} MINOR2=${RELEASE_MINOR2:-46008df}
PORT0=${SIFT_PORT:-56130} ROWS=100000
GUARDED='src test scripts Makefile pg_sift.control pg_sift--*.sql'
CRASH='terminated by signal (4|6|7|8|11):|TRAP: '	# signals 9 and 15 are what the crash tests send on purpose

# name:tier:what it proves
JOBS=(
	build:1:"both flavors build with no warning"
	werror-gcc:1:"warnings are errors with gcc"
	werror-clang:1:"warnings are errors with clang"
	portable:1:"the portable core includes only the host header"
	ci-quick:1:"scripts/ci.sh quick (the shrunk suites), normal build"
	coverage:1:"every row of the coverage table holds"
	ci-sanitize:2:"scripts/ci.sh quick on the ASan+UBSan build"
	pg17:2:"scripts/ci.sh quick on PostgreSQL 17 (container)"
	race:2:"the visibility and VACUUM race tests"
	gate-bm25:2:"BM25 instructions per operation against the base"
	gate-col:2:"column scan instructions per question against the gated ref"
	fuzz:2:"60 s of the column chunk and BM25 decoders"
	coldiff:2:"aggregate pushdown against PostgreSQL: distinct, pre-aggregates, top-N, search, sets"
	sqlite:2:"the SQLite build and smoke test"
	mariadb:2:"the MariaDB build and smoke test (one series)"
	upgrade:3:"2.0 -> 3.0 in place, a format 2 column index asks for REINDEX"
	package:3:"a fresh CREATE EXTENSION from the install target"
)

key() {
	local dirty
	dirty=$(cd "$REPO" && git status --porcelain -- $GUARDED)
	[[ -z $dirty || -n $LIST ]] || { echo "release: uncommitted changes in the gated paths:" >&2; echo "$dirty" >&2; return 1; }
	( cd "$REPO" && git rev-parse HEAD:src HEAD:test HEAD:scripts HEAD:Makefile && git ls-files -s pg_sift.control 'pg_sift--*.sql' ) | sha256sum | cut -c1-16
	[[ -z $dirty ]] || echo "(uncommitted changes in the gated paths: this key is of HEAD without them)" >&2
}
KEY=$(key) || exit 1
HEAD_SHA=$(git -C "$REPO" rev-parse --short HEAD)

if [[ -n $LIST ]]; then
	echo "tree $KEY (head $HEAD_SHA), stamp $STATE/$KEY: $([[ -f $STATE/$KEY ]] && echo present || echo absent)"
	for j in "${JOBS[@]}"; do IFS=: read -r n t w <<<"$j"; [[ $n =~ $ONLY ]] && printf 'tier %s  %-13s %s\n' "$t" "$n" "$w"; done
	exit 0
fi
if [[ -f $STATE/$KEY && -z $FORCE && $ONLY == . ]]; then
	echo "release: tree $KEY is already gated: $(tail -1 "$STATE/$KEY")"
	exit 0
fi
# the whole run in 2 cores and 12 GB, whatever the caller did
if [[ -z ${SIFT_RELEASE_SCOPED:-} ]] && command -v systemd-run >/dev/null; then
	exec systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 -p OOMPolicy=continue -p CPUQuota=200% \
		env SIFT_RELEASE_SCOPED=1 "$0" "$@"
fi

W=$(mktemp -d "${RELEASE_TMP:-/tmp}/sift-release.XXXXXX")
LOGS=$W/logs T=$W/tree
mkdir -p "$LOGS" "$T"
trap '[[ -n $KEEP ]] || find "$W" -mindepth 1 -maxdepth 1 ! -name logs -exec rm -rf {} +' EXIT
git -C "$REPO" archive HEAD | tar -x -C "$T"
cd "$T" || exit 1
export SIFT_PORT=$PORT0 LC_ALL=C

tree() { mkdir -p "$2" && git -C "$REPO" archive "$1" | tar -x -C "$2"; }	# tree REF DIR: another ref, no checkout
pgt() { local d=$1 tmp=$2 port=$3; shift 3; ( cd "$d" && SIFT_TMP=$tmp SIFT_PORT=$port scripts/pgtest.sh "$@" ); }
q() { local sock=$1 port=$2 db=$3; shift 3; psql -X -q -At -v ON_ERROR_STOP=1 -h "$sock" -p "$port" -U postgres -d "$db" "$@"; }
ci() { # NAME [ENV...]: scripts/ci.sh quick in the scratch of NAME; RERUN, the failed parts of the first run, narrows it
	local n=$1; shift
	[[ -z ${RERUN:-} ]] || export SIFT_CI_JOBS="^(${RERUN// /|})$"
	env "$@" KEEP=1 SIFT_CI_TMP=$W/$n SIFT_TMP=$W/$n-home SIFT_LANES=2 nice scripts/ci.sh quick
}
warnings() { ! grep -q 'warning:' "$1" || { grep 'warning:' "$1" | head -5; return 1; }; }

job_build() {
	local f
	for f in 0 1; do
		SIFT_TMP=$W/build SIFT_TESTING=$f nice scripts/pgtest.sh install >"$W/build.$f.log" 2>&1 || { tail -20 "$W/build.$f.log"; return 1; }
		warnings "$W/build.$f.log" || return 1
		make USE_PGXS=1 clean >/dev/null; rm -f .build-flavor
	done
}
werror() { make USE_PGXS=1 clean >/dev/null; rm -f .build-flavor; nice make -j2 USE_PGXS=1 CC="$1" COPT="-Werror ${2:-}" SIFT_TESTING=1; }
job_werror_gcc() { werror gcc; }
# clang: PostgreSQL's CFLAGS name gcc-only warnings; its walker callbacks are cast as PostgreSQL's own macros do (it builds with
# -Wno-cast-function-type-strict); a header with a flexible array member inside a struct is the segment writer's since 2.0 (GNU extension)
job_werror_clang() { werror clang "-Wno-unknown-warning-option -Wno-cast-function-type-strict -Wno-gnu-variable-sized-type-not-at-end"; }
job_portable() { scripts/check-portable.sh; }
job_ci_quick() { ci ci-quick; }
job_ci_sanitize() { ci ci-sanitize SANITIZE=1; }
job_coverage() {
	export SIFT_TMP=$W/coverage
	nice scripts/pgtest.sh install >/dev/null && scripts/pgtest.sh start >/dev/null
	scripts/coverage.sh --check
	scripts/pgtest.sh stop >/dev/null
}
job_pg17() {
	local out=$W/pg17; mkdir -p "$out"; cp -r "$T" "$out/src"; chmod -R a+rwX "$out"
	docker run --rm --cpus 2 --memory 12g -v "$out/src:/src" -e HOME=/tmp/h -e CRASH="$CRASH" postgres:17-bookworm bash -c '
		set -e
		mkdir -p /tmp/h; chown postgres /tmp/h
		apt-get update -qq >/dev/null
		DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential postgresql-server-dev-17 liblz4-dev libzstd-dev perl libipc-run-perl python3 >/dev/null
		chown -R postgres /src /usr/lib/postgresql/17 /usr/share/postgresql/17; mkdir -p /tmp/ci && chown postgres /tmp/ci
		rc=0; su postgres -c "cd /src && KEEP=1 SIFT_CI_TMP=/tmp/ci/work SIFT_TMP=/tmp/ci/home SIFT_LANES=2 scripts/ci.sh quick" || rc=$?
		grep -rIEh "$CRASH" /tmp/ci | head -3 | sed "s/^/CRASH: /"
		exit $rc'
}
job_race() {
	export SIFT_TMP=$W/race SIFT_TESTING=1
	local t
	for t in vacuum_count_stress vacuum_count_dead vacuum_count_snapshot vacuum_agg_stress col_vacuum_scan topn_vacuum_scan; do
		scripts/pgtest.sh tap "$t" || return 1
	done
}
job_gate_bm25() {
	local out
	tree "$BM25_BASE" "$W/gate-bm25/base"
	out=$(nice bench/steps/cpu_gate.sh --reps 5 --max-reps 11 --budget 2400 --build "base=$W/gate-bm25/base" --build "new=$T" 2>&1)
	echo "$out"
	grep -q '^cell ' <<<"$out" || { echo "the gate printed no table"; return 1; }
	local bad
	bad=$(awk -v ok="^($BM25_ACCEPT)\$" '$NF == "REGRESS" && $1 !~ ok { print $1, $5 }' <<<"$out")
	[[ -z $bad ]] || { echo "cells above 1.02 that are not accepted: $bad"; return 1; }
}
last_head() { # the head of the newest stamp
	local f; f=$(ls -t "$STATE"/* 2>/dev/null | head -1)
	[[ -z $f ]] || sed -n 's/.* head \([0-9a-f]*\) .*/\1/p' "$f" | tail -1
}
# the store of docs/BENCHMARKS.md at 100000 rows on both layouts: sift_col on a heap table (db col) and sift_table (db tbl)
store_cluster() { # DIR TMP PORT
	local d=$1 tmp=$2 port=$3 sock=$2/sock kind ddl
	pgt "$d" "$tmp" "$port" install >/dev/null && pgt "$d" "$tmp" "$port" start >/dev/null || return 1
	for db in src col tbl; do q "$sock" "$port" postgres -c "CREATE DATABASE $db"; done
	q "$sock" "$port" src -v n=$ROWS -f "$T/bench/sql/store/gen.sql" >/dev/null 2>&1
	for kind in col tbl; do
		ddl=$T/bench/sql/store/sift.sql; [[ $kind == tbl ]] && ddl=$T/bench/sql/store/sift_table.sql
		sec() { awk -v s="$1" '/^--@ /{cur=$2; next} cur == s' "$ddl"; }
		sec table | q "$sock" "$port" $kind -f -
		q "$sock" "$port" src -c "COPY orders_src TO STDOUT" | q "$sock" "$port" $kind -c "COPY orders FROM STDIN"
		sec indexes | q "$sock" "$port" $kind -f -
		q "$sock" "$port" $kind -c "VACUUM ANALYZE orders"
	done
}
job_gate_col() {
	local ref=${RELEASE_COL_REF:-$(last_head)} db sql ratio bad=
	[[ -n $ref ]] || { echo "no reference: set RELEASE_COL_REF to a gated ref"; return 1; }
	tree "$ref" "$W/gate-col/ref"
	store_cluster "$W/gate-col/ref" "$W/gate-col/rt" $((PORT0 + 1)) && store_cluster "$T" "$W/gate-col/nt" $((PORT0 + 2)) || return 1
	local Q=(
		"SELECT id, customer_id, country, category, status, amount, quantity, created_at, title FROM orders WHERE id = 1234"
		"SELECT country, count(*) FROM orders GROUP BY country"
		"SELECT date_trunc('month', created_at)::date m, sum(amount) FROM orders GROUP BY m"
		"SELECT date_trunc('month', created_at)::date m, count(DISTINCT customer_id) FROM orders GROUP BY m"
		"SELECT category, sum(amount) s FROM orders GROUP BY category ORDER BY s DESC LIMIT 10"
		"SELECT count(*) FROM orders WHERE amount > 100 AND created_at > timestamp '2025-12-01'")
	for db in col tbl; do
		for sql in "${Q[@]}"; do
			ratio=$(bench/cpu-query.sh -n 1000 -r 3 "$sql" "$W/gate-col/nt/sock:$((PORT0 + 2))/$db" "$W/gate-col/rt/sock:$((PORT0 + 1))/$db" | tee /dev/stderr | sed -n 's/^NEW\/REF: instructions \([0-9.]*\),.*/\1/p')
			printf '%s %s %s\n' "$db" "$ratio" "${sql:0:60}"
			[[ $db == col ]] || continue
			awk -v r="$ratio" -v m="$COL_NOTE" 'BEGIN { exit !(r > m) }' && echo "accepted: size work ($ratio > $COL_NOTE): ${sql:0:60}"
			awk -v r="$ratio" -v m="$COL_MAX" 'BEGIN { exit !(r > m) }' && bad=1
		done
	done
	for d in rt nt; do pgt "$T" "$W/gate-col/$d" $PORT0 stop >/dev/null 2>&1; done
	[[ -z $bad ]] || { echo "a sift_col question is above $COL_MAX of the reference"; return 1; }
}
job_fuzz() { SIFT_TMP=$W/fuzz FZ_SECS=60 scripts/fuzz.sh colchunk bm25; }
job_coldiff() {
	export SIFT_TMP=$W/coldiff
	nice scripts/pgtest.sh install >/dev/null && scripts/pgtest.sh start >/dev/null
	scripts/coldiff.sh --suite fixtures --corpus 'distinct|preagg|topn|search|sets|table|tbl' --fixture-rows 20000 --scenarios all
	scripts/pgtest.sh stop >/dev/null
}
job_sqlite() {
	cd sqlite && nice make -j2 && nice make -j2 isa-check && python3 -I tests/smoke.py build/sift "$(cat VERSION)" && nice make -j2 span-test col-smoke
}
job_mariadb() { SERIES=${RELEASE_MARIADB_SERIES:-11.8} DOCKER_CPUS=2 "$REPO/scripts/package-mariadb.sh" all "$HEAD_SHA" "$W/mariadb"; }
job_upgrade() {
	local U=$W/upgrade port=$((PORT0 + 3)) sock=$W/upgrade/sock ans
	export SIFT_TESTING=
	local DATA="CREATE TABLE docs (id int PRIMARY KEY, g int, body text);
		INSERT INTO docs SELECT i, i % 7, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, 20000) i;
		CREATE INDEX docs_idx ON docs USING sift (body);"
	local ANS="SELECT (SELECT count(*) FROM docs WHERE body @@> 'alpha3 beta4') || ':' || (SELECT count(*) FROM docs WHERE body @@> 'beta7 NOT alpha1');"
	tree "$OLD" "$W/upgrade-old"; tree HEAD "$W/upgrade-new"
	# 2.0 writes a BM25 index; 3.0 replaces the files in the same cluster and takes the extension up
	pgt "$W/upgrade-old" "$U" $port install >/dev/null && pgt "$W/upgrade-old" "$U" $port start >/dev/null || return 1
	q "$sock" $port sift -c "CREATE EXTENSION pg_sift" -c "$DATA"
	ans=$(q "$sock" $port sift -c "$ANS"); echo "2.0: $ans"
	pgt "$W/upgrade-old" "$U" $port stop >/dev/null
	pgt "$W/upgrade-new" "$U" $port install >/dev/null && pgt "$W/upgrade-new" "$U" $port start >/dev/null || return 1
	q "$sock" $port sift -c "ALTER EXTENSION pg_sift UPDATE TO '3.0'"
	[[ $(q "$sock" $port sift -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'") == 3.0 ]] || { echo "extversion is not 3.0"; return 1; }
	[[ $(q "$sock" $port sift -c "$ANS") == "$ans" ]] || { echo "the answers changed by the update"; return 1; }
	q "$sock" $port sift -c "SELECT sift.verify('docs_idx', true)" -c "REINDEX INDEX docs_idx"
	[[ $(q "$sock" $port sift -c "$ANS") == "$ans" ]] || { echo "the answers changed by REINDEX"; return 1; }
	q "$sock" $port sift -c "CREATE INDEX docs_col ON docs USING sift_col (id, g)" -c "SELECT sift.col_verify('docs_col', true)"
	pgt "$W/upgrade-new" "$U" $port stop >/dev/null
	upgrade_minor2
}
# a column index of the last head that wrote format 2: 3.0 asks for REINDEX, and REINDEX makes it answer
upgrade_minor2() {
	local U=$W/upgrade2 port=$((PORT0 + 4)) sock=$W/upgrade2/sock err ans
	local AGG="SET sift.enable_agg = on; SET cpu_tuple_cost = 0.5; SELECT g, count(*), sum(n) FROM m GROUP BY g ORDER BY g;"
	tree "$MINOR2" "$W/upgrade2-old"
	pgt "$W/upgrade2-old" "$U" $port install >/dev/null && pgt "$W/upgrade2-old" "$U" $port start >/dev/null || return 1
	q "$sock" $port sift -c "CREATE EXTENSION pg_sift" -c "CREATE TABLE m (id int, g int, n int)" \
		-c "INSERT INTO m SELECT i, i % 9, i % 100 FROM generate_series(1, 30000) i" -c "CREATE INDEX m_col ON m USING sift_col (id, g, n)" -c "VACUUM m"
	ans=$(q "$sock" $port sift -c "$AGG"); pgt "$W/upgrade2-old" "$U" $port stop >/dev/null
	pgt "$W/upgrade-new" "$U" $port install >/dev/null && pgt "$W/upgrade-new" "$U" $port start >/dev/null || return 1
	err=$(q "$sock" $port sift -c "$AGG" 2>&1) && { echo "a format 2 column index was read"; return 1; }
	grep -q REINDEX <<<"$err" || { echo "no REINDEX hint: $err"; return 1; }
	q "$sock" $port sift -c "REINDEX INDEX m_col"
	[[ $(q "$sock" $port sift -c "$AGG") == "$ans" ]] || { echo "the answers changed by REINDEX"; return 1; }
	pgt "$W/upgrade-new" "$U" $port stop >/dev/null
}
job_package() {
	local U=$W/package port=$((PORT0 + 5)) sock=$W/package/sock
	make USE_PGXS=1 clean >/dev/null; rm -f .build-flavor
	nice make -j2 USE_PGXS=1 SIFT_TESTING= >/dev/null && make USE_PGXS=1 SIFT_TESTING= DESTDIR="$U/root" install >/dev/null || return 1
	[[ -f $U/root$(pg_config --sharedir)/extension/pg_sift--3.0.sql && -f $U/root$(pg_config --pkglibdir)/pg_sift.so ]] || { echo "the install target did not write the extension"; return 1; }
	export SIFT_TESTING=
	pgt "$T" "$U" $port start >/dev/null || return 1
	q "$sock" $port sift -c "CREATE EXTENSION pg_sift" -c "CREATE TABLE d (id int, t text)" -c "INSERT INTO d SELECT i, 'w' || i % 10 FROM generate_series(1, 1000) i" \
		-c "CREATE INDEX d_idx ON d USING sift (t)" -c "CREATE INDEX d_col ON d USING sift_col (id)"
	[[ $(q "$sock" $port sift -c "SELECT count(*) FROM d WHERE t @@> 'w3'") == 100 ]] || { echo "wrong answer from the installed package"; return 1; }
	pgt "$T" "$U" $port stop >/dev/null
}

known_flake() { # NAME FAILED-PARTS: every part matches a line of scripts/release.flakes; prints the causes
	local p cause
	for p in ${2:-$1}; do
		cause=$(awk -F'\t' -v p="$p" 'p ~ $1 { print $2; exit }' "$REPO/scripts/release.flakes" 2>/dev/null)
		[[ -n $cause ]] || return 1
		printf '%s ' "$cause"
	done
}
crash_scan() { grep -rIEh "$CRASH" "$W/$1" "$W/$1"-* "$LOGS/$1.log" 2>/dev/null | head -3; }
run_once() { # NAME -> rc; log LOGS/NAME.log
	local n=$1 rc c
	( set -e; "job_${n//-/_}" ) >"$LOGS/$n.log" 2>&1
	rc=$?
	find "$W" -path '*/data/postmaster.pid' -printf '%h\n' | while read -r d; do pg_ctl -D "$d" stop -m immediate >/dev/null 2>&1; done
	c=$(crash_scan "$n")
	[[ -z $c ]] || { echo "server crash:"$'\n'"$c" >>"$LOGS/$n.log"; rc=99; }
	[[ -n $KEEP ]] || find "$W" -mindepth 1 -maxdepth 1 -name "$n*" -exec rm -rf {} +
	return $rc
}

SUMMARY=() T0=$SECONDS NFLAKE=0
for j in "${JOBS[@]}"; do
	IFS=: read -r n tier _ <<<"$j"
	[[ $n =~ $ONLY ]] || continue
	t0=$SECONDS
	run_once "$n"; rc=$?
	status=pass
	if (( rc )); then
		parts=$(grep -hE '^ *failed:' "$LOGS/$n.log" | sed -E 's/^ *failed: *//' | tr ' ' '\n' | sort -u | tr '\n' ' ')
		cp "$LOGS/$n.log" "$LOGS/$n.first.log"
		echo "release: $n failed (rc $rc${parts:+, $parts}), once more alone" >&2
		RERUN=$parts run_once "$n"; rc2=$?
		if (( rc2 == 0 )) && causes=$(known_flake "$n" "$parts"); then status="pass (known flake: $causes)"; NFLAKE=$((NFLAKE + 1))
		else status=FAIL; fi
	fi
	SUMMARY+=("$(printf '%-13s tier %s  %5ds  %s' "$n" "$tier" $((SECONDS - t0)) "$status")")
	echo "${SUMMARY[-1]}" >&2
	[[ $status != FAIL ]] || break
done

printf '%s\n' "${SUMMARY[@]}"
WALL=$((SECONDS - T0))
if [[ ${SUMMARY[*]} == *FAIL* ]]; then
	echo "release: FAIL tree $KEY head $HEAD_SHA wall ${WALL}s, logs $LOGS"
	exit 1
fi
LINE="release: PASS tree $KEY head $HEAD_SHA wall ${WALL}s jobs ${#SUMMARY[@]} known-flakes $NFLAKE date $(date +%F)"
echo "$LINE"
if [[ $ONLY == . ]]; then mkdir -p "$STATE" && { printf '%s\n' "${SUMMARY[@]}"; echo "$LINE"; } >"$STATE/$KEY"; fi
