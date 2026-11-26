#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A sift_col index of Format 2 that tracks its changes is told of every delete by its row trigger, and the build that left the trigger in place vouches for it (SiftColFormatOps.deletes_heard): a
# read of a part whose inserters ended before the snapshot then leaves the heap alone (EXPLAIN heap_vet is 0) and still answers what the heap answers. The word ends, and the reads check the
# heap again, when the trigger is disabled or made again (another catalog row), when a statement ran with the trigger muted, when the snapshot began before the build ended or is a
# SERIALIZABLE one (its predicate locks are the heap's), and while a statement of the transaction that ended versions has not been heard yet. Every read here is compared with the heap plan.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 33
# Format 2: the deletes its row trigger hears (SiftColFormatOps.deletes_heard), which a Format 1 index does not vouch for
node_init th 5 "sift.test_create_format1 = off
autovacuum = off
max_parallel_workers_per_gather = 0
sift.async_maintenance = off"
node_start th || bail "start failed"
sql th "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

Q="SELECT g, count(*) AS c, sum(v) AS s FROM t GROUP BY g"
D="SELECT coalesce(md5(string_agg(r::text, ',' ORDER BY g)), '') FROM ($Q) r"

# exact [BEGIN...]: the aggregate over the index answers the rows of the heap plan, in one session (the statements before it set the transaction up)
exact() {
	local a b
	a=$(sql th "LOAD 'pg_sift'" "$@" "SET sift.enable_agg = on" "$D") || return 1
	b=$(sql th "LOAD 'pg_sift'" "$@" "SET sift.enable_agg = off" "$D") || return 1
	out="$a"$'\n'"$b"
	[[ -n $a && $a == "$b" ]]
}
# vet [SETUP...]: the rows of the aggregate that the heap was asked about (EXPLAIN of the statement)
vet() {
	local n
	n=$(sql th "LOAD 'pg_sift'" "$@" "SET sift.enable_agg = on" "EXPLAIN (ANALYZE, COSTS OFF) $Q" | sed -n 's/.*heap_vet=\([0-9][0-9]*\).*/\1/p' | head -n 1)
	echo "${n:-0}"
}
heap_free() { out=$(vet "$@"); [[ $out == 0 ]]; }
heap_asked() { out=$(vet "$@"); ((out > 0)); }
uses_index() { out=$(sql th "LOAD 'pg_sift'" "SET sift.enable_agg = on" "EXPLAIN (COSTS OFF) $Q"); [[ $out == *SiftAggV6* ]]; }
delete_rows() { sql th "$@" >/dev/null || bail "delete failed: $*"; }
reindex() { sql th "REINDEX INDEX t_col" >/dev/null || bail "reindex failed"; }

sql th "CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO t SELECT i, i % 16, i % 100 FROM generate_series(1, 20000) i" \
	"CREATE INDEX t_col ON t USING sift_col (id, g, v) WITH (track_changes = 'ctid')" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "load failed"

check "the aggregate reads the index" uses_index
check "clean: the heap is not asked" heap_free

delete_rows "DELETE FROM t WHERE id % 10 = 0"
check "10% deleted: the rows are those of the heap" exact
check "10% deleted: the heap is not asked" heap_free

delete_rows "SET session_replication_role = replica" "DELETE FROM t WHERE id % 10 = 1"
check "deleted under the replica role (the trigger fires for every role): the rows are those of the heap" exact
check "deleted under the replica role: the heap is not asked" heap_free

# another session: a delete that has not ended, then rolled back, then committed
coproc OTHER { psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock th)" -p "$(port th)" -U postgres -d "$(node_db th)"; }
other_pid=$OTHER_PID
printf '%s\n' "BEGIN;" "DELETE FROM t WHERE id % 10 = 2;" "\\echo READY" >&"${OTHER[1]}"
read -r -u "${OTHER[0]}" ready
check "a delete another session has not ended: the rows are those of the heap" exact
check "a delete another session has not ended: the heap is not asked" heap_free
printf '%s\n' "ROLLBACK;" "\\echo READY" >&"${OTHER[1]}"
read -r -u "${OTHER[0]}" ready
check "the delete rolled back: the rows are those of the heap" exact
check "the delete rolled back: the heap is not asked" heap_free
printf '%s\n' "DELETE FROM t WHERE id % 10 = 2;" "\\echo READY" >&"${OTHER[1]}"
read -r -u "${OTHER[0]}" ready
check "the delete committed: the rows are those of the heap" exact
check "the delete committed: the heap is not asked" heap_free
printf '%s\n' "\\q" >&"${OTHER[1]}"
wait "$other_pid" 2>/dev/null

sql th "UPDATE t SET v = v + 1 WHERE id % 10 = 3" "DELETE FROM t WHERE id % 20 = 3" >/dev/null || bail "update failed"
check "a column delta and a delete of the same rows: the rows are those of the heap" exact

# a read inside the statement that ends the versions: the trigger has not heard them when the function reads
sql th "CREATE TABLE seen (ok bool)" "CREATE FUNCTION peek() RETURNS bool LANGUAGE plpgsql AS \$f\$
DECLARE a text; b text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE \$q\$$D\$q\$ INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE \$q\$$D\$q\$ INTO b;
	INSERT INTO seen VALUES (a = b);
	RETURN true;
END \$f\$" >/dev/null || bail "function failed"
delete_rows "LOAD 'pg_sift'" "DELETE FROM t WHERE id % 10 = 4 AND id < 500 AND peek()"
peeked_rows() { out=$(sql th "SELECT count(*), bool_and(ok) FROM seen"); [[ $out == [1-9]*"|t" ]]; }
check "a read of the table while the statement that deletes from it runs: the rows are those of the heap" peeked_rows
check "the table after that statement: the rows are those of the heap" exact

check "REPEATABLE READ: the rows are those of the heap" exact "BEGIN ISOLATION LEVEL REPEATABLE READ"
check "REPEATABLE READ: the heap is not asked" heap_free "BEGIN ISOLATION LEVEL REPEATABLE READ"
check "SERIALIZABLE: the rows are those of the heap" exact "BEGIN ISOLATION LEVEL SERIALIZABLE"
check "SERIALIZABLE: the heap is asked, which takes its predicate locks" heap_asked "BEGIN ISOLATION LEVEL SERIALIZABLE"

# a snapshot that began before the rebuild ended: rows inserted since are not its rows, though the new index holds them as written
coproc OLD { psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock th)" -p "$(port th)" -U postgres -d "$(node_db th)"; }
old_pid=$OLD_PID
printf '%s\n' "LOAD 'pg_sift';" "BEGIN ISOLATION LEVEL REPEATABLE READ;" "SELECT 1;" "\\echo READY" >&"${OLD[1]}"
read -r -u "${OLD[0]}" snapshot_taken
read -r -u "${OLD[0]}" ready
sql th "INSERT INTO t SELECT i, i % 16, 1 FROM generate_series(30001, 30500) i" "REINDEX INDEX t_col" >/dev/null || bail "insert and reindex failed"
printf '%s\n' "SET sift.enable_agg = on;" "$D;" "SET sift.enable_agg = off;" "$D;" "\\echo READY" >&"${OLD[1]}"
read -r -u "${OLD[0]}" with_index
read -r -u "${OLD[0]}" with_heap
read -r -u "${OLD[0]}" ready
printf '%s\n' "ROLLBACK;" "\\q" >&"${OLD[1]}"
wait "$old_pid" 2>/dev/null
old_snapshot_exact() { out="$with_index"$'\n'"$with_heap"; [[ -n $with_index && $with_index == "$with_heap" ]]; }
check "a snapshot that began before the REINDEX: the rows are those of the heap" old_snapshot_exact

delete_rows "DELETE FROM t WHERE id % 10 = 5"
check "after the REINDEX: the rows are those of the heap" exact
check "after the REINDEX: the heap is not asked" heap_free

delete_rows "ALTER TABLE t DISABLE TRIGGER ALL" "DELETE FROM t WHERE id % 10 = 6"
check "the trigger disabled, rows deleted: the rows are those of the heap" exact
check "the trigger disabled: the heap is asked" heap_asked
delete_rows "ALTER TABLE t ENABLE TRIGGER ALL"
check "the trigger enabled again: the heap is still asked (it is another row of the catalog)" heap_asked
delete_rows "DELETE FROM t WHERE id % 10 = 7"
check "the trigger enabled again, rows deleted: the rows are those of the heap" exact
reindex
delete_rows "DELETE FROM t WHERE id % 10 = 8"
check "REINDEX after that: the rows are those of the heap" exact
check "REINDEX after that: the heap is not asked" heap_free

delete_rows "ALTER INDEX t_col SET (track_changes = 'off')" "DELETE FROM t WHERE id % 10 = 9 AND id < 8000" "ALTER INDEX t_col SET (track_changes = 'ctid')"
check "track_changes off and on again, rows deleted in between: the rows are those of the heap" exact
check "track_changes off and on again: the heap is asked" heap_asked
reindex

delete_rows "SET sift.test_no_track = on" "DELETE FROM t WHERE id % 100 = 11"
check "a statement with the trigger muted: the rows are those of the heap" exact
check "a statement with the trigger muted: the heap is asked" heap_asked
reindex
check "the index built again: the heap is not asked" heap_free

finish
