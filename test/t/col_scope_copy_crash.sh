#!/usr/bin/env bash
# kill -9 of the postmaster and every backend in the middle of a transaction that changed facts and pairs of a scope copy: the copy and the change of the tables are one
# transaction, so after recovery the uncommitted change is gone from both and sift.scope_verify finds nothing (a checkpoint before the crash writes the pages of the
# uncommitted rows, the heap and the sift index of the copy, to disk; a committed transaction before it stays). Then random transactions of several clients (facts, pairs and
# data-modifying CTEs that change both) are cut by a crash at random moments, SIFT_SCOPE_CRASH_ITERS times (default 6, 3 quick).
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_scope.bash"

ITERS=$(knob SIFT_SCOPE_CRASH_ITERS 6 3)
plan $((ITERS + 9))
node_init crash 48 "checkpoint_timeout = 30s
autovacuum = off"
node_start crash || bail "start failed"
scope_setup crash || bail "setup failed"
D=$(dir crash)
ok "setup"
check "the copies are exact" scope_exact crash
COUNTS="SELECT (SELECT count(*) FROM facts) || ',' || (SELECT count(*) FROM pairs) || ',' || (SELECT count(*) FROM cflat) || ',' || (SELECT count(*) FROM cpart)"

# an open transaction: statements go to a psql that reads a fifo
open_txn() {
	rm -f "$D/in"; mkfifo "$D/in"
	psql -X -At -q -h "$D/sock" -p "$(port crash)" -U postgres -d postgres -f "$D/in" >"$D/open.out" 2>&1 &
	OPEN_PID=$!
	exec 3>"$D/in"
	cat >&3 <<'EOF'
BEGIN;
INSERT INTO facts SELECT 500000 + i, 1 + i % 45, now(), i % 5, i, 'open' FROM generate_series(1, 300) i;
INSERT INTO pairs VALUES (45, 7), (44, 7), (10, 8), (20, 8);
UPDATE facts SET amount = amount + 1000, dim_a_id = 1 + dim_a_id % 45 WHERE id % 9 = 0;
DELETE FROM pairs WHERE dim_a_id IN (1, 9) AND scope_id <= 3;
DELETE FROM facts WHERE id % 13 = 0;
WITH p AS (DELETE FROM pairs WHERE scope_id = 2 RETURNING 1) INSERT INTO facts SELECT 600000 + i, 1 + i % 45, now(), 1, i, 'cte' FROM generate_series(1, 50) i;
SELECT 'statements done';
EOF
	wait_for 'grep -q "statements done" "$D/open.out"' 60
}
crash_open() { crash crash; exec 3>&-; wait "$OPEN_PID" 2>/dev/null; node_start crash; }

sql crash "BEGIN" "INSERT INTO facts SELECT 400000 + i, 1 + i % 45, now(), 2, i, 'kept' FROM generate_series(1, 100) i" "INSERT INTO pairs VALUES (45, 1)" "COMMIT" >/dev/null
sql crash "CHECKPOINT" >/dev/null
BEFORE=$(sql crash "$COUNTS")

open_txn
check "an open transaction changed the facts, the pairs and the copies" test "$(sql crash "SELECT count(*) FROM pg_stat_activity WHERE state = 'idle in transaction'")" == 1
crash_open
check "crash with the transaction open: it is gone from the tables and the copies, the committed one stays" test "$(sql crash "$COUNTS")" == "$BEFORE"
check "... and the copies are exact" scope_exact crash

open_txn
sql crash "CHECKPOINT" >/dev/null
crash_open
check "crash after a checkpoint that wrote the pages of the open transaction: the same" test "$(sql crash "$COUNTS")" == "$BEFORE"
check "... and the copies are exact" scope_exact crash
edit_after() { sql crash "INSERT INTO facts VALUES (700001, 45, now(), 1, 1, 'after')" "UPDATE pairs SET scope_id = 8 WHERE dim_a_id = 45 AND scope_id = 7" "DELETE FROM facts WHERE id = 700001" >/dev/null && scope_exact crash; }
check "a change after the recovery keeps the triggers: a fact, a pair that moves, a delete" edit_after

FILES=($(scope_files crash))
for i in $(seq "$ITERS"); do
	crash_cycle crash "iteration $i" 3 6 "${FILES[@]}" || break
	out=$(sql crash "SELECT * FROM sift.scope_verify('cflat') LIMIT 5" "SELECT * FROM sift.scope_verify('cpart') LIMIT 5")
	check "iteration $i: verified after the crash" scope_exact crash
done
check "the sift indexes of the copies verify" test -z "$(sql crash "SELECT sift.col_verify('cflat_col', true)" 2>&1)"
finish
