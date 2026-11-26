#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The log of a BM25 index is a record for a page of docs, not for each doc (bm25/wbuf.c stages the docs of a statement): a COPY of 16,384 docs into an indexed table and an UPDATE that writes a new
# version of every row without changing the text both log at most 145 B and 0.05 records of the index for a doc, with full_page_writes on and a checkpoint before them, and a SELECT over an open write
# buffer of 2,000 docs and more, with the same checkpoint before it, logs nothing.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5
ROWS=16384
DOC="'w' || (hashint4(i) & 63) || ' x' || (hashint4(i + 1) & 2047) || ' y' || (hashint4(i + 2) & 2047) || ' z' || (hashint4(i + 3) & 4095) || ' v' || (hashint4(i + 5) & 1023)"

node_init sw 73 "autovacuum = off
sift.async_maintenance = off
full_page_writes = on
wal_compression = off
shared_buffers = 32MB
max_wal_size = 4GB
checkpoint_timeout = 1h"
node_start sw || bail "start failed"
sql sw "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_walinspect" >/dev/null || bail "extension failed"
sql sw "CREATE TABLE d (id int, other int, body text)" "CREATE INDEX d_ix ON d USING sift (body)" "CREATE INDEX d_other ON d (other)" \
	"COPY (SELECT i, i, $DOC FROM generate_series(1, $ROWS) i) TO '$(dir sw)/d.tsv'" >/dev/null || bail "table failed"

# window STATEMENT: the bytes and the records of the window of the log that are not the heap's or a btree's, and the size of the index in the end; one line "bytes records disk"
window() {
	sql sw <<EOF | tail -n 1
CHECKPOINT;
SELECT pg_current_wal_insert_lsn() AS l0 \gset
$1;
SELECT pg_current_wal_insert_lsn() AS l1 \gset
SELECT pg_current_xact_id() IS NOT NULL AS flushed \gset
SELECT coalesce(sum(record_length), 0) || ' ' || count(*) || ' ' || pg_relation_size('d_ix')
FROM pg_get_wal_records_info(:'l0', :'l1') WHERE resource_manager NOT IN ('Heap', 'Heap2', 'Btree', 'Standby', 'Transaction');
EOF
}

out=$(window "COPY d FROM '$(dir sw)/d.tsv'")
check "a COPY of $ROWS docs logs at most 145 B and 0.05 records of the index for a doc (bytes records disk: $out)" \
	awk -v v="$out" -v n="$ROWS" 'BEGIN { split(v, a, " "); exit !(a[1] / n <= 145 && a[2] / n <= 0.05) }'
check "... and the index takes at most 125 B for a doc" awk -v v="$out" -v n="$ROWS" 'BEGIN { split(v, a, " "); exit !(a[3] / n <= 125) }'
check "... and finds every doc it was given" test "$(sql sw "SELECT count(*) FROM d WHERE body @@> 'w5'")" = "$(sql sw "SELECT count(*) FROM d WHERE body ~ '\mw5\M'")"
sql sw "SELECT sift.verify('d_ix', true)" >/dev/null || bail "verify failed"

sql sw "VACUUM d" >/dev/null || bail "vacuum failed"
out=$(window "UPDATE d SET other = other + 1")
check "an UPDATE of every row that keeps the text logs at most 145 B and 0.05 records of the index for a doc (bytes records disk: $out)" \
	awk -v v="$out" -v n="$ROWS" 'BEGIN { split(v, a, " "); exit !(a[1] / n <= 145 && a[2] / n <= 0.05) }'

sql sw "TRUNCATE d" "INSERT INTO d SELECT i, i, $DOC FROM generate_series(1, 3000) i" "SET enable_indexonlyscan = off" "SELECT count(body) FROM d" >/dev/null || bail "buffer failed"
sql sw "SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), pg_current_wal_insert_lsn())" >/dev/null
out=$(sql sw <<'EOF' | tail -n 1
CHECKPOINT;
SELECT pg_current_wal_insert_lsn() AS l0 \gset
DO $$ BEGIN FOR i IN 1..200 LOOP PERFORM sum(id) FROM d WHERE body @@> 'w3'; END LOOP; END $$;
SELECT pg_wal_lsn_diff(pg_current_wal_insert_lsn(), :'l0');
EOF
)
check "200 searches over an open write buffer of 3,000 docs log nothing (bytes: $out)" test "$out" = 0
finish
