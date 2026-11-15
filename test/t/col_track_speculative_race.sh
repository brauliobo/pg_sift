#!/usr/bin/env bash
# An INSERT .. ON CONFLICT whose speculative insertion loses a race leaves no row in the index of a partition. The session that upserts is new: it loaded the library while the statement ran
# (a partition's index is opened when the first row is routed to it), so it did not hear the statement start, and a trigger that runs SQL before the row reaches the index starts
# statements that it does hear. The index of the partition is ahead of the unique index in the table, so its entry is made while the heap row is still speculative; then another session's
# insertion of the same key commits, the unique index finds it, and the heap row is killed without a word to the sift_col index. A read that trusted the entry would count the row.
# The race is ordered, not timed: the upserting session waits on an advisory lock inside the expression of an index that comes first (a SQL function made VOLATILE after the index, so that the
# planner inlines it and no statement starts), the other session inserts the key and the first one goes on when the lock is released.
. "$(dirname "$0")/lib.bash"

plan 10
node_init sr 84 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start sr || bail "start failed"
D=$(dir sr)

sql sr "CREATE EXTENSION pg_sift" \
	"CREATE FUNCTION slow(v int) RETURNS int IMMUTABLE LANGUAGE sql AS \$\$ SELECT v + length(CASE WHEN v = 4242 THEN pg_advisory_xact_lock(20984, 1)::text ELSE '' END) \$\$" \
	"CREATE FUNCTION look() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN PERFORM count(*) FROM pg_class; RETURN NEW; END \$\$" >/dev/null || bail "setup failed"

for n in 5 6; do
	sql sr "CREATE FUNCTION up$n() RETURNS trigger LANGUAGE plpgsql AS \$\$ BEGIN INSERT INTO c$n VALUES (5002, 1, 4242) ON CONFLICT (id) DO UPDATE SET v = c$n.v + 1; RETURN NEW; END \$\$" >/dev/null
done
# table NAME [STATEMENT...]: two partitions, the lock index, the sift_col indexes and, last, the unique index (REINDEX CONCURRENTLY gives it a later OID); then the statements
table() {
	local t=$1
	shift
	sql sr "CREATE TABLE $t (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) PARTITION BY RANGE (id)" \
		"CREATE TABLE ${t}0 PARTITION OF $t FOR VALUES FROM (0) TO (1000) WITH (autovacuum_enabled = off)" \
		"CREATE TABLE ${t}1 PARTITION OF $t FOR VALUES FROM (1000) TO (100000) WITH (autovacuum_enabled = off)" \
		"INSERT INTO $t SELECT i * 3, i % 7, i FROM generate_series(0, 1999) i" \
		"CREATE INDEX ${t}_slow ON $t (slow(v))" \
		"CREATE INDEX ${t}0_c ON ${t}0 USING sift_col (id, g, v)" "CREATE INDEX ${t}1_c ON ${t}1 USING sift_col (id, g, v)" \
		"REINDEX INDEX CONCURRENTLY ${t}0_pkey" "REINDEX INDEX CONCURRENTLY ${t}1_pkey" "VACUUM $t" "$@" >/dev/null || bail "table $t failed"
}
trigger() { echo "CREATE TRIGGER t BEFORE INSERT ON $1 FOR EACH ROW EXECUTE FUNCTION look()"; }

# race TABLE STATEMENT: the statement, in a new session, upserts the key 5002 of TABLE and loses to the insertion of the key by another session
race() {
	rm -f "$D"/{locked,release}
	{
		echo "SELECT pg_advisory_lock(20984, 1);"
		echo "\\! touch '$D/locked'"
		echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
	} | sql sr >/dev/null &
	local hp=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
	sql sr "$2" >/dev/null &
	local up=$!
	for _ in $(seq 400); do [[ $(sql sr "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] && break; sleep 0.05; done
	sql sr "INSERT INTO $1 VALUES (5002, 2, 7)" >/dev/null
	touch "$D/release"
	wait "$hp" "$up"
}
Q() { echo "SELECT count(*) || ':' || sum(v) FROM $1"; }
sift() { sql sr "SET seq_page_cost = 10000" "$(Q "$1")"; }
heap() { sql sr "SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_indexonlyscan = off" "SET enable_bitmapscan = off" "$(Q "$1")"; }

# no trigger: no statement starts before the row reaches the index; a trigger on each partition, on the table, and DO NOTHING with triggers on the partitions
table c1
table c2 "$(trigger c20)" "$(trigger c21)"
table c3 "$(trigger c3)"
table c4 "$(trigger c40)" "$(trigger c41)"
table k5 "CREATE TRIGGER t BEFORE INSERT ON k50 FOR EACH ROW EXECUTE FUNCTION up5()" "CREATE TRIGGER t BEFORE INSERT ON k51 FOR EACH ROW EXECUTE FUNCTION up5()"
table k6 "CREATE TRIGGER t BEFORE INSERT ON k6 FOR EACH STATEMENT EXECUTE FUNCTION up6()"
table c5
table c6
sql sr "ALTER FUNCTION slow(int) VOLATILE" >/dev/null
upsert() { echo "INSERT INTO $1 VALUES (5002, 1, 4242) ON CONFLICT (id) $2"; }
race c1 "$(upsert c1 "DO UPDATE SET v = c1.v + 1")"
race c2 "$(upsert c2 "DO UPDATE SET v = c2.v + 1")"
race c3 "$(upsert c3 "DO UPDATE SET v = c3.v + 1")"
race c4 "$(upsert c4 "DO NOTHING")"
# a COPY whose trigger upserts: the upsert is nested in a statement that the library did not hear start (BEFORE ROW: the library loaded when the COPY routed its row, so the upsert is heard;
# BEFORE STATEMENT: it loaded in the upsert, which started before it)
printf '6000\t1\t1\n' >"$D/copy.dat"
race c5 "COPY k5 FROM '$D/copy.dat'"
race c6 "COPY k6 FROM '$D/copy.dat'"
for c in c1 c2 c3 c4 c5 c6 k5 k6; do
	a=$(heap "$c")
	check "$c: the count of the sift_col read is the heap's ($a)" test "$(sift "$c")" == "$a"
done
check "the upsert lost the race and updated the row of the other session" test "$(sql sr "SELECT v FROM c2 WHERE id = 5002")" == 8
check "... or left it as it was (DO NOTHING)" test "$(sql sr "SELECT v FROM c4 WHERE id = 5002")" == 7
finish
