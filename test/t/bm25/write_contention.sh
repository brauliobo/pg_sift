#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The writers of a BM25 index cost what their docs do: 50 ranked searches that meet dead docs, each followed by DISCARD ALL, leave at most one cache of dead docs; a plain INSERT reads no heap
# page besides the one it writes (only an ON CONFLICT statement asks the heap whether a doc is speculative); one INSERT of docs interleaved over 32 partitions logs at most 0.05 records and 80 B of
# the indexes for a doc (every partition stages its docs, none is flushed for another); and 16 writers of 1 KB docs beside 8 searchers leave an index that verifies, open and merged, and finds every
# doc, though an append counts its page in the header of the buffer only when the header is free.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 7
SECS=$(knob SIFT_WC_SECS 4 2)

node_init wc 87 "autovacuum = off
sift.async_maintenance = off
shared_buffers = 64MB
max_wal_size = 2GB
checkpoint_timeout = 1h"
node_start wc || bail "start failed"
D=$(dir wc)
sql wc "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_walinspect" >/dev/null || bail "extension failed"

sql wc "CREATE TABLE dk (id int, body text) WITH (autovacuum_enabled = off)" "INSERT INTO dk SELECT i, 'alpha w' || i % 50 FROM generate_series(1, 5000) i" \
	"CREATE INDEX dk_i ON dk USING sift (body)" "DELETE FROM dk WHERE id % 2 = 0" >/dev/null || bail "dead docs setup failed"
ctx=$({ for _ in $(seq 50); do
	echo "SELECT count(*) FROM (SELECT id FROM dk WHERE body @@> 'alpha' ORDER BY sift.score(ctid) DESC LIMIT 10) s \\g /dev/null"
	echo "DISCARD ALL;"
done; echo "SELECT count(*) FROM pg_backend_memory_contexts WHERE name = 'sift cache dead docs';"; } | sql wc | tail -n 1)
check "50 searches that meet dead docs, each with DISCARD ALL, leave at most one cache of dead docs ($ctx)" test "$ctx" -le 1

sql wc "CREATE TABLE hh (id int, body text)" "CREATE INDEX hh_i ON hh USING sift (body)" "INSERT INTO hh VALUES (0, 'warm')" >/dev/null || bail "heap setup failed"
hits=$(sql wc "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON, TIMING off, SUMMARY off) INSERT INTO hh SELECT i, 'w' || i % 97 FROM generate_series(1, 10000) i" |
	grep -o '"Shared Hit Blocks": [0-9]*' | head -n 1 | grep -o '[0-9]*$')
check "an INSERT of 10,000 docs touches at most 1.2 buffers for a doc, its heap page included ($hits)" test "$hits" -le 12000
sql wc "CREATE TABLE hc (id int PRIMARY KEY, body text)" "CREATE INDEX hc_i ON hc USING sift (body)" "INSERT INTO hc SELECT i, 'kept' FROM generate_series(1, 400, 2) i" \
	"INSERT INTO hc SELECT i, 'lost' FROM generate_series(1, 400) i ON CONFLICT (id) DO NOTHING" >/dev/null || bail "conflict failed"
check "... and an ON CONFLICT stages the docs it inserts, of a non-unique index too" \
	test "$(sql wc "SELECT count(*) FILTER (WHERE body @@> 'lost') || ' ' || count(*) FILTER (WHERE body @@> 'kept') FROM hc")" = "200 200"

sql wc "CREATE TABLE pp (id int, body text) PARTITION BY LIST ((id % 32))" >/dev/null || bail "partitioned table failed"
for g in $(seq 0 31); do sql wc "CREATE TABLE pp_$g PARTITION OF pp FOR VALUES IN ($g)" >/dev/null || bail "partition $g failed"; done
sql wc "CREATE INDEX pp_i ON pp USING sift (body)" >/dev/null || bail "partitioned index failed"
out=$(sql wc <<'EOF' | tail -n 1
CHECKPOINT;
SELECT pg_current_wal_insert_lsn() AS l0 \gset
INSERT INTO pp SELECT i, 'w' || (i % 97) || ' x' || (i % 13) FROM generate_series(1, 32000) i;
SELECT pg_current_wal_insert_lsn() AS l1 \gset
SELECT coalesce(sum(record_length), 0) || ' ' || count(*) FROM pg_get_wal_records_info(:'l0', :'l1') WHERE resource_manager NOT IN ('Heap', 'Heap2', 'Btree', 'Standby', 'Transaction');
EOF
)
check "an INSERT of 32,000 docs over 32 partitions logs at most 80 B and 0.05 records of the indexes for a doc (bytes records: $out)" \
	awk -v v="$out" 'BEGIN { split(v, a, " "); exit !(a[1] / 32000 <= 80 && a[2] / 32000 <= 0.05) }'

sql wc "CREATE TABLE cw (id bigserial, body text)" "CREATE INDEX cw_i ON cw USING sift (body) WITH (write_buffer_limit = 100000)" \
	"INSERT INTO cw (body) SELECT 'w' || i % 5000 || ' w' || i * 7 % 5000 FROM generate_series(1, 20000) i" >/dev/null || bail "contention setup failed"
printf '%s\n' "INSERT INTO cw (body) SELECT string_agg('w' || (random() * 5000)::int, ' ') FROM generate_series(1, 150);" >"$D/w.sql"
printf '%s\n' "\\set t random(1, 5000)" "SELECT id FROM cw WHERE body @@> ('w' || :t) ORDER BY sift.score(ctid) DESC LIMIT 10;" >"$D/r.sql"
pgbench_bg wc 16 "$SECS" -f "$D/w.sql"
wpid=$PGBENCH_PID
pgbench -n -h "$D/sock" -p "$(port wc)" -U postgres -c 8 -j 4 -T "$SECS" -f "$D/r.sql" postgres >"$D/read.log" 2>&1
wait "$wpid"
diag "writers: $(grep -E '^tps' "$D/pgbench.log"); searchers: $(grep -E '^(tps|latency average)' "$D/read.log" | tr '\n' ' ')"
check "16 writers beside 8 searchers leave an index that verifies" sql wc "SELECT sift.verify('cw_i', true)"
check "... that finds every doc" test "$(sql wc "SELECT count(*) FROM cw WHERE body @@> 'w7'")" = "$(sql wc "SELECT count(*) FROM cw WHERE body ~ '\mw7\M'")"
sql wc "SELECT sift.merge('cw_i', true)" >/dev/null || bail "merge failed"
check "... and merged verifies with every row a doc" test "$(sql wc "SELECT sift.verify('cw_i', true) IS NOT NULL AND (SELECT sum(ndocs) FROM sift.segments('cw_i')) = (SELECT count(*) FROM cw)")" = t
finish
