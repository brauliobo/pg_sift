#!/usr/bin/env bash
# The merge of a sift_col index cuts the free end it leaves, and a cut that finds the index locked (a transaction that planned a query holds an AccessShareLock on every
# index of the table until it ends) is not lost: the next call of sift.col_merge, which has nothing to merge, cuts it. Before, only a call that merged looked at the end.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

plan 5
node_init mc 12 "autovacuum = off
sift.async_maintenance = off"
node_start mc || bail "start failed"
sql mc "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, key int, kind text, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"CREATE INDEX t_idx ON t USING sift_col (id, key, kind, amount) WITH (point_tid_columns = 'off')" >/dev/null || bail "setup failed"
# batches that each become a segment of their own: the merge writes one segment into the lowest free blocks and frees the last ones of the file
for b in 0 1 2 3 4 5; do
	sql mc "INSERT INTO t SELECT $b * 20000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 50) FROM generate_series(1, 20000) g" "SELECT sift.col_promote('t_idx')" >/dev/null || bail "batch $b failed"
done
size() { sql mc "SELECT pg_relation_size('t_idx') / 8192"; }
before=$(size)

# a transaction that has planned a query on the table: its locks are held until it ends, its snapshot is not
coproc HOLD { psql -X -q -At -h "$(dir mc)/sock" -p "$(port mc)" -U postgres -d postgres 2>&1; }
holder=$HOLD_PID   # bash unsets HOLD_PID when it reaps the coprocess, which is at the first wait or at its exit
echo "BEGIN; SELECT count(*) FROM t;" >&"${HOLD[1]}"
read -r -t 20 _ <&"${HOLD[0]}"
check "the holder has locked the index" test "$(sql mc "SELECT count(*) FROM pg_locks l JOIN pg_class c ON c.oid = l.relation WHERE c.relname = 't_idx' AND l.mode = 'AccessShareLock'")" -ge 1

merged=$(sql mc "SELECT sift.col_merge('t_idx', true)")
check "the merge ran" test "$merged" -ge 1
busy=$(size)
check "the lock held the cut off: the file is as long as the merge left it" test "$busy" -ge "$before"

echo "COMMIT;" >&"${HOLD[1]}"
echo "\\q" >&"${HOLD[1]}"
wait "$holder" 2>/dev/null || true
check "the next call has nothing to merge" test "$(sql mc "SELECT sift.col_merge('t_idx')")" == 0
free=$(size)
check "... and cuts the end: $busy -> $free blocks" test "$free" -lt "$((busy * 3 / 4))"
sql mc "SELECT sift.col_verify('t_idx', true)" >/dev/null 2>&1 || not_ok "verify"
finish
