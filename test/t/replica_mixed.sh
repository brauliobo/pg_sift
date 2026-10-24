#!/usr/bin/env bash
# A physical standby on another library than its primary, in both directions of a rolling update, with PostgreSQL's own WAL (the standby replays
# pages; it needs no sift code for that) and the library each node loads:
#   up:   the primary runs the newest 3.2 release, the standby this tree (the standby is updated first, as usual): the standby answers the queries of the
#         BM25, sift_col and sift_table objects the release wrote and keeps writing as the primary does (inserts, updates, deletes, VACUUM), as the primary answers
#         them and as the plain plan does, sift.verify and sift.col_verify are clean on it.
#   down: the primary runs this tree at the column format minor the release reads (sift.col_format_minor = 6, the setting of the rollback in docs/UPGRADING.md
#         section 4), the standby the release: the same checks.
# The release is built from its tag by scripts/old-lib-check.sh (PostgreSQL 18).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=$(git -C "$REPO" tag -l 'v3.2.*' --sort=-v:refname | head -n 1)
[[ -n $ref ]] || { echo '1..0 # SKIP no release tag v3.2.*'; exit 0; }
newroot=$ROOT
build_old "$ref"; oldroot=$OLDROOT
plan 18

QS=("SELECT g, count(*), sum(n) FROM docs GROUP BY g ORDER BY g" "SELECT count(*), sum(n) FROM docs WHERE n < 40" "SELECT count(*) FROM docs WHERE body @@> 'alpha3 beta4'"
	"SELECT g, count(*), sum(n) FROM tbl GROUP BY g ORDER BY g")
# answers NODE MODE: the queries in the plan mode "idx" (the indexes) or "plain" (the heap and the table by their own plans), one line each
answers() {
	local mode=$1 node=$2 q out=()
	local -a set=("SET sift.enable_agg = on" "SET seq_page_cost = 10000")
	[[ $mode == plain ]] && set=("SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off")
	for q in "${QS[@]}"; do out+=("$(sql "$node" "${set[@]}" "$q" 2>&1 | tr '\n' ' ')"); done
	printf '%s\n' "${out[@]}"
}
verify() { test -z "$(sql "$1" "SELECT sift.verify('docs_idx', true)" "SELECT sift.col_verify('docs_col', true)" 2>&1 | grep -v '^$' | grep -ivx 't\|true')"; }
write_load() { # NODE: more writes of every kind
	sql "$1" "INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(20001, 20600) i" \
		"UPDATE docs SET n = n + 1 WHERE id % 97 = 0" "DELETE FROM docs WHERE id % 89 = 0" \
		"INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(20001, 20600) i" "UPDATE tbl SET n = n + 1 WHERE id % 97 = 0" "DELETE FROM tbl WHERE id % 89 = 0" \
		"VACUUM docs" "VACUUM tbl" >/dev/null
}

# run DIRECTION PRIMARY_ROOT STANDBY_ROOT [primary conf]
run() {
	local dir=$1 proot=$2 sroot=$3 pconf=${4:-} p=pm_$1 s=ps_$1 off=$([[ $1 == up ]] && echo 4 || echo 8)
	node_init "$p" "$off" "wal_level = replica
max_wal_senders = 4
autovacuum = off
sift.async_maintenance = off${pconf:+
$pconf}"
	use_root "$p" "$proot"
	node_start "$p" || bail "$dir: primary start failed"
	sql "$p" "CREATE EXTENSION pg_sift" \
		"CREATE TABLE docs (id int PRIMARY KEY, g int, n int, body text)" \
		"INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, 12000) i" \
		"CREATE INDEX docs_idx ON docs USING sift (body)" "CREATE INDEX docs_col ON docs USING sift_col (id, g, n)" \
		"INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 3 || ' beta' || i % 7 FROM generate_series(12001, 12600) i" \
		"CREATE TABLE tbl (id int, g int, n int) USING sift_table" "INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(1, 12000) i" "VACUUM tbl" \
		"INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(12001, 12600) i" "VACUUM docs" >/dev/null || bail "$dir: setup failed"
	node_clone "$p" "$s" $((off + 1)) "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
	use_root "$s" "$sroot"
	node_start "$s" || bail "$dir: standby start failed"
	check "$dir: the standby streams from a base backup of the other library's primary" catchup "$p" "$s"
	check "$dir: the standby verifies the indexes" verify "$s"
	agree_std() { # DESCRIPTION: the standby's answers equal the primary's and the plain plan's
		local pa sa sp
		pa=$(answers idx "$p"); sa=$(answers idx "$s"); sp=$(answers plain "$s")
		[[ $sa != *ERROR* && $pa != *ERROR* ]] || bail "$1: a query failed: $sa$pa"
		check "$1: standby = primary" test "$sa" == "$pa"
		check "$1: standby indexes = standby plain plan" test "$sa" == "$sp"
	}
	agree_std "$dir"
	write_load "$p"
	check "$dir: the standby replays the writes" catchup "$p" "$s"
	agree_std "$dir, after writes"
	check "$dir: the indexes verify on the standby after the writes" verify "$s"
	check "$dir: and on the primary" verify "$p"
	node_stop "$s"
	node_stop "$p"
}

run up "$oldroot" "$newroot"
run down "$newroot" "$oldroot" "sift.col_format_minor = 6"
finish
