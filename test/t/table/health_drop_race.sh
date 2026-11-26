#!/usr/bin/env bash
# tap: pooled
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# sift.health() and sift.status() while other sessions create and drop sift_tables, heap tables with a sift_col index and partitions: a relation that a DROP took away between the
# catalog read of the statement and the lock of the relation is no row, not an error (sift.worker_state answers NULL for it, as it does for a relation another session holds locked).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 3
node_init hd 32 "autovacuum = off
max_connections = 100"
node_start hd || bail "start failed"
D=$(dir hd)
sql hd "CREATE EXTENSION IF NOT EXISTS pg_sift" >/dev/null || bail "setup failed"
PSQL=(psql -X -q -h "$(node_sock hd)" -p "$(port hd)" -U postgres -d "$(node_db hd)")

churn() {
	local n=$1 end=$((SECONDS + $2))
	while ((SECONDS < end)); do
		"${PSQL[@]}" >/dev/null 2>&1 <<-SQL
			CREATE TABLE hd_t$n (id int, v int) USING sift_table;
			INSERT INTO hd_t$n SELECT g, g FROM generate_series(1, 50) g;
			CREATE TABLE hd_h$n (id int, v int);
			CREATE INDEX hd_h${n}_c ON hd_h$n USING sift_col (id, v);
			INSERT INTO hd_h$n SELECT g, g FROM generate_series(1, 50) g;
			DROP TABLE hd_t$n;
			DROP TABLE hd_h$n;
			CREATE TABLE hd_p$n (id int, t int) PARTITION BY RANGE (t);
			CREATE TABLE hd_p${n}_0 PARTITION OF hd_p$n FOR VALUES FROM (0) TO (10) USING sift_table;
			INSERT INTO hd_p$n VALUES (1, 1);
			DROP TABLE hd_p${n}_0;
			DROP TABLE hd_p$n;
		SQL
	done
}

SECS=$(knob HEALTH_DROP_SECS 12 6)
echo "SELECT count(*) FROM sift.health();" >"$D/health.pgb"
echo "SELECT count(*) FROM sift.status();" >"$D/status.pgb"
for n in 1 2 3 4 5 6; do churn "$n" "$SECS" & done
pgbench -n -c 3 -j 3 -T "$SECS" -f "$D/health.pgb" -f "$D/status.pgb" "${PSQL[@]:3}" >"$D/bench.out" 2>&1
wait
out=$(grep -E 'actually processed|failed transactions|error' "$D/bench.out" | head -n 8)
check "no call of sift.health() or sift.status() fails while relations are dropped" grep -q "^number of failed transactions: 0 " "$D/bench.out"
check "no client aborts" test "$(grep -c 'aborted' "$D/bench.out")" == 0
check "sift.health() runs after the drops" test -n "$(sql hd "SELECT count(*) FROM sift.health()")"
finish
