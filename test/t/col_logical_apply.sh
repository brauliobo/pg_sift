#!/usr/bin/env bash
# The apply worker of a logical subscriber writes the changes of the publisher to tables with a sift_col index (with track_changes off and on) and a sift (BM25) index: a transaction of
# several statements with rows after its first (the rows a sift_col index buffers), updates, deletes and an aborted one. Every index verifies on the subscriber, the groups read by the
# column index are the heap's, and the tracked index does not read without the heap there (the apply worker fires no statement trigger).
. "$(dirname "$0")/lib.bash"

plan 8
ROWS=$(knob SIFT_APPLY_ROWS 3000 1500)
node_init p 42 "wal_level = logical
max_wal_senders = 5
max_replication_slots = 5
autovacuum = off
max_logical_replication_workers = 4
max_worker_processes = 16"
node_start p || bail "start failed"
node_init s 43 "wal_level = logical
max_replication_slots = 5
autovacuum = off
max_logical_replication_workers = 4
max_worker_processes = 16"
node_start s || bail "subscriber start failed"
export PGOPTIONS="-c client_min_messages=warning"

DDL="CREATE EXTENSION pg_sift
CREATE TABLE lp (id int PRIMARY KEY, g int NOT NULL, v int, title text) WITH (autovacuum_enabled = false)
CREATE TABLE lt (id int PRIMARY KEY, g int NOT NULL, v int, title text) WITH (autovacuum_enabled = false)
CREATE TABLE lb (id int PRIMARY KEY, g int NOT NULL, v int, title text) WITH (autovacuum_enabled = false)
CREATE INDEX lp_c ON lp USING sift_col (id, g, v) WITH (track_changes = 'off')
CREATE INDEX lt_c ON lt USING sift_col (id, g, v)
CREATE INDEX lb_b ON lb USING sift (title)"
while IFS= read -r q; do sql p "$q" >/dev/null || bail "ddl failed on the publisher: $q"; sql s "$q" >/dev/null || bail "ddl failed on the subscriber: $q"; done <<<"$DDL"
sql p "INSERT INTO lp SELECT i, i % 7, i, 'alpha ' || (i % 13) FROM generate_series(1, $ROWS) i" \
	"INSERT INTO lt SELECT i, i % 7, i, 'alpha ' || (i % 13) FROM generate_series(1, $ROWS) i" \
	"INSERT INTO lb SELECT i, i % 7, i, 'alpha ' || (i % 13) FROM generate_series(1, $ROWS) i" "CREATE PUBLICATION pb FOR TABLE lp, lt, lb" >/dev/null || bail "publisher setup failed"
sql s "CREATE SUBSCRIPTION sb CONNECTION '$(conninfo p)' PUBLICATION pb" >/dev/null || bail "subscribe failed"

synced() { # the subscriber holds the rows of the publisher: the sums agree on every table
	local want have t
	for t in lp lt lb; do
		want=$(sql p "SELECT count(*) || ':' || coalesce(sum(v), 0) || ':' || coalesce(sum(id), 0) FROM $t")
		have=$(sql s "SELECT count(*) || ':' || coalesce(sum(v), 0) || ':' || coalesce(sum(id), 0) FROM $t")
		[[ $want == "$have" ]] || return 1
	done
}
wait_synced() { for _ in $(seq 300); do synced && return 0; sleep 0.2; done; return 1; }
check "the subscriber has the initial rows" wait_synced

for t in lp lt lb; do
	sql p "BEGIN" "INSERT INTO $t SELECT i, i % 7, i, 'beta ' || (i % 5) FROM generate_series($((ROWS + 1)), $((ROWS + 300))) i" \
		"INSERT INTO $t SELECT i, i % 7, i, 'beta ' || (i % 5) FROM generate_series($((ROWS + 301)), $((ROWS + 600))) i" \
		"UPDATE $t SET v = v + 1000, title = 'gamma ' || (id % 3) WHERE id % 11 = 0" "UPDATE $t SET id = id + 500000 WHERE id % 17 = 0" "DELETE FROM $t WHERE id % 19 = 0" "COMMIT" \
		"BEGIN" "INSERT INTO $t SELECT i, 0, i, 'aborted' FROM generate_series(900001, 900100) i" "ROLLBACK" >/dev/null
done
check "the subscriber follows the changes" wait_synced

verify_s() { # $1 table, $2 index, $3 function
	[[ -z $(sql s "SELECT sift.$3('$2', true)" 2>&1) ]]
}
groups() { # $1 node, $2 table, $3 enable_agg
	sql "$1" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $3" "SET seq_page_cost = 10000" \
		"SELECT md5(string_agg(g || ':' || c || ':' || s, ',' ORDER BY g)) FROM (SELECT g, count(*) c, sum(v) s FROM $2 GROUP BY g) x" | tail -n 1
}
check "the sift_col index with track_changes off verifies on the subscriber" verify_s lp lp_c col_verify
check "the sift_col index with track_changes on verifies on the subscriber" verify_s lt lt_c col_verify
check "the sift index verifies on the subscriber" verify_s lb lb_b verify
agree() { [[ $(groups s "$1" on) == "$(groups s "$1" off)" && $(groups s "$1" on) == "$(groups p "$1" off)" ]]; }
check "the groups the column indexes read on the subscriber are the heap's and the publisher's" agree lp
check "the groups of the tracked index on the subscriber are the heap's and the publisher's" agree lt
check "the tracked index does not read without the heap on the subscriber" test "$(sql s "SELECT heap_free FROM sift.col_tracking('lt_c')")" == f
finish
