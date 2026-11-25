#!/usr/bin/env bash
# What a DELETE leaves to the BM25 index of a table when sift.async_maintenance is on: a lone segment of which a quarter of the documents are deleted is rewritten by the VACUUM that
# marks them (exactly a quarter counts, one under it does not), the segments that the rewrite limit of that VACUUM left are rewritten by the maintenance in the background, the
# maintenance settles (no worker, no queued job, no WAL written), and the scans of the index, a bitmap scan, the count and the ranked scan, each count in pg_stat_user_indexes.idx_scan.
. "$(dirname "$0")/lib.bash"

plan 12
ROWS=16384

node_init bd 61 "autovacuum = off
sift.async_maintenance = on"
node_start bd || bail "start failed"
sql bd "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

# table NAME ROWS SEGMENTS WHERE LIMIT: ROWS documents of 20 terms in SEGMENTS segments, those of WHERE deleted, and the VACUUM that finds them rewrites at most LIMIT megabytes
table() {
	sql bd "CREATE TABLE $1 (id int8, body text) WITH (autovacuum_enabled = off)" \
		"INSERT INTO $1 SELECT g, (SELECT string_agg('w' || ((g * 7 + j * 13) % 3000) || ' x' || ((g + j) % 500), ' ') FROM generate_series(1, 20) j) FROM generate_series(1, $2) g" \
		"CREATE INDEX ${1}_idx ON $1 USING sift (body) WITH (build_segments = $3)" \
		"DELETE FROM $1 WHERE $4" "SET sift.vacuum_rewrite_limit = $5" "VACUUM $1" >/dev/null || bail "table $1 failed"
}
dead() { sql bd "SELECT coalesce(sum(dead_docs), 0) FROM sift.segments('$1_idx'::regclass)"; }
rewritten() { [[ $(sql bd "SELECT coalesce(bool_and(dead_docs * 100 < ndocs * 25), false) FROM sift.segments('$1_idx'::regclass) WHERE kind = 'immutable'") == t ]]; }
idle() { [[ $(sql bd "SELECT count(*) FROM sift.activity() WHERE kind IN ('worker', 'queued')") == 0 ]]; }

# a quarter of the documents: more than the rewrite asks for in the VACUUM that marks them, and with a worker for what the VACUUM leaves
table quarter $ROWS 1 "id <= $((ROWS / 4))" 1024
check "a segment a quarter of whose documents are deleted is rewritten" wait_for 'rewritten quarter' 30
check "its documents are all live" test "$(dead quarter)" == 0
check "the maintenance that follows has no job left" wait_for idle 30
check "and writes no WAL once it is idle" wait_stable bd "SELECT pg_current_wal_lsn()::text" 30

# one document under a quarter stays as it is: the index is not rewritten for less
table below $ROWS 1 "id <= $((ROWS / 4 - 1))" 1024
check "a segment with one document less than a quarter deleted is not rewritten" test "$(dead below)" == $((ROWS / 4 - 1))
check "and settles with no job" wait_for idle 30

# a VACUUM that stops at its rewrite limit: the segments it left, half of whose documents are deleted, are rewritten without another one
table limited 60000 6 "id % 2 = 0" 1
check "the segments that the rewrite limit of the VACUUM left are rewritten in the background" wait_for 'rewritten limited' 30
check "which settles with no job" wait_for idle 30
check "and writes no WAL once it is idle" wait_stable bd "SELECT pg_current_wal_lsn()::text" 30

# the scans count in pg_stat_user_indexes
scans() { sql bd "SELECT pg_stat_force_next_flush()" >/dev/null; sql bd "SELECT idx_scan FROM pg_stat_user_indexes WHERE indexrelname = 'quarter_idx'"; }
counted() {
	local before after
	before=$(scans)
	sql bd "$1" >/dev/null || return 1
	after=$(scans)
	((after > before))
}
check "a bitmap scan counts as an index scan" counted "SET sift.enable_scan = off; SET enable_seqscan = off; SELECT id FROM quarter WHERE body @@> 'x1'"
check "the count counts as an index scan" counted "SELECT count(*) FROM quarter WHERE body @@> 'x1'"
check "the ranked scan counts as an index scan" counted "SELECT id FROM quarter WHERE body @@> 'x1' ORDER BY sift.score(ctid) DESC LIMIT 5"

finish
