#!/usr/bin/env bash
# 16 clients write a sift_col index over a heap table (60% INSERT, 30% UPDATE, 10% DELETE) while a session flushes its slab (sift._test_f2_slab_flush) again and again. The slab of a
# partition is a chain of references to heap tuples (small writes) or of batches (a write the chain of references refuses), and the writers turn it over from one to the other: a flush
# decides which it holds, and must read that chain while it holds the lock of the partition, not one it looked at before (it once read a chain of references as one of batches: "the slab
# of partition 1 is damaged", the index quarantined). The flusher is held before it starts (sift.test_phase_gate, SIFT_F2_GATE_SLAB_FLUSH = 15) for 30 ms at a time, while the writers go on.
# No statement fails, no index is quarantined or damaged, the index verifies, and the column scan answers what the heap does.
. "$(dirname "$0")/lib.bash"

plan 7
SECS=$(knob SIFT_SLAB_SECS 8 4)
GATE=7300
node_init sc 24 "autovacuum = off
sift.log_min_duration = 0"
node_start sc || bail "start failed"
sql sc "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE facts (id bigint, key int, kind text, amount numeric(10,2), pad text) WITH (autovacuum_enabled = false)" \
	"CREATE SEQUENCE facts_seq START 30001" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 40) FROM generate_series(1, 30000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"
D=$(dir sc)
cat >"$D/ins.sql" <<'SQL'
INSERT INTO facts SELECT i, i % 1000, 'cat_' || (i % 8), i % 997, repeat('x', 40) FROM (SELECT nextval('facts_seq') AS i) n;
SQL
cat >"$D/upd.sql" <<'SQL'
\set rid random(1, :n)
UPDATE facts SET amount = amount + 1 WHERE id = :rid;
SQL
cat >"$D/del.sql" <<'SQL'
\set rid random(1, :n)
DELETE FROM facts WHERE id = :rid;
SQL

# the gate is shut for 30 ms at a time (a session holds its advisory lock) and open for the time the next session takes to connect
gatekeeper() { while [[ ! -e $D/stop ]]; do sql sc "SELECT pg_advisory_lock($((GATE + 15)))" "SELECT pg_sleep(0.03)" >/dev/null 2>&1; done; }
flusher() {
	while [[ ! -e $D/stop ]]; do
		sql sc "SET sift.test_phase_gate = $GATE" "SELECT count(*) FROM (SELECT sift._test_f2_slab_flush('facts_idx'::regclass, 1::int8, NULL::int8) FROM generate_series(1, 20)) f" >>"$D/flush.n" 2>>"$D/flusher.err" || echo failed >>"$D/flusher.err"
	done
}

rm -f "$D/stop" "$D/flush.n"
: >"$D/flusher.err"
gatekeeper & gp=$!
flusher & fp=$!
pgbench_bg sc 16 "$SECS" -D n=60000 -f "$D/ins.sql"@6 -f "$D/upd.sql"@3 -f "$D/del.sql"@1
wait "$PGBENCH_PID"; bench=$?
touch "$D/stop"
wait "$gp" "$fp"
diag "$(grep -E 'transactions actually processed|tps' "$D/pgbench.log" | tr '\n' ' ') flush sessions: $(wc -l <"$D/flush.n" | tr -d ' ')"
diag "first damage in the log: $(grep -a -m2 -A1 -E 'is damaged|is corrupted' "$D/postgres.log" | cut -c1-240)"$'\n'"flusher: $(head -n 3 "$D/flusher.err" | cut -c1-240)"
check "pgbench ran its 16 clients to the end" test "$bench" -eq 0
check "no statement of the 16 clients failed" test "$(grep -ac -E '^(pgbench: error|ERROR)|failed transactions: [1-9]' "$D/pgbench.log")" -eq 0
check "the flusher ran and met no damage" test ! -s "$D/flusher.err" -a "$(wc -l <"$D/flush.n")" -ge 5
check "no slab is damaged and no index quarantined (log of the server)" test "$(grep -ac -E 'is damaged|is corrupted|quarantine' "$D/postgres.log")" -eq 0
sql sc "VACUUM facts" >/dev/null
check "afterwards the index verifies" test -z "$(sql sc "SELECT sift.col_verify('facts_idx', true)" 2>&1)"
check "the column scan counts and sums the rows of the heap" test "$(sql sc "SET sift.enable_agg = on" "SELECT count(*), sum(amount) FROM facts" | tail -1)" == "$(sql sc "SET sift.enable_agg = off" "SELECT count(*), sum(amount) FROM facts" | tail -1)"
check "and groups them as the heap does" test "$(sql sc "SET sift.enable_agg = on" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1)" == \
	"$(sql sc "SET sift.enable_agg = off" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1)"
finish
