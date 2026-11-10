#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup) of a sift_col index whose order arrays are compact (order_layout = 'compact': the chunks keep the bucket of a key and read the
# exact one from the heap row): inserts, updates that change the key, deletes, VACUUM, promotion and merge on the primary replay to an index that verifies on the hot
# standby and whose ordered reads (ASC and DESC, an OFFSET, a range) return what its heap returns and what the primary returns, while readers run on the standby. The
# standby is then promoted and takes writes. SIFT_COL_REPLICA_SECS (default 15, 3 quick).
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_col.bash"

SECS=$(knob SIFT_COL_REPLICA_SECS 15 3)
plan 11
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off
sift.test_col_gorder_ends = 64
sift.test_limits = 'gord_seek_pct=0'"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"SELECT setseed(0.42)" \
	"CREATE TABLE orders (id bigserial PRIMARY KEY, ts timestamp, qty bigint)" \
	"INSERT INTO orders (ts, qty) SELECT TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', i FROM generate_series(1, 30000) i" \
	"CREATE INDEX o_col ON orders USING sift_col (id, ts, qty) WITH (write_buffer_limit = 64, order_columns = 'ts', order_array_columns = 'ts', order_layout = 'compact')" \
	"CREATE FUNCTION ordered_same() RETURNS boolean LANGUAGE plpgsql AS \$\$
	DECLARE q text; a text; b text; qs text[] = ARRAY[
		'SELECT ts AS k FROM orders ORDER BY ts LIMIT 40 OFFSET 900',
		'SELECT ts AS k FROM orders ORDER BY ts DESC LIMIT 40 OFFSET 5000',
		'SELECT ts AS k FROM orders WHERE ts >= TIMESTAMP ''2026-04-01'' ORDER BY ts LIMIT 40 OFFSET 3000',
		'SELECT ts AS k FROM orders WHERE ts <= TIMESTAMP ''2026-08-01'' ORDER BY ts DESC LIMIT 100'];
	BEGIN
		FOREACH q IN ARRAY qs LOOP
			PERFORM set_config('sift.enable_agg', 'on', true);
			EXECUTE 'SELECT string_agg(k::text, '','') FROM (' || q || ') s' INTO a;
			PERFORM set_config('sift.enable_agg', 'off', true);
			EXECUTE 'SELECT string_agg(k::text, '','') FROM (' || q || ') s' INTO b;
			IF a IS DISTINCT FROM b THEN RETURN false; END IF;
		END LOOP;
		RETURN true;
	END \$\$" >/dev/null || bail "setup failed"
check "the segments of the index are compact" test "$(sql primary "SELECT count(*) FROM sift.col_segments('o_col') WHERE kind = 'immutable' AND format = 41")" -gt 0

node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

P=$(dir primary)
cat >"$P/ins.sql" <<'SQL'
INSERT INTO orders (ts, qty) SELECT TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', i FROM generate_series(1, 20) i;
SQL
cat >"$P/upd.sql" <<'SQL'
\set id random(1, 100000)
UPDATE orders SET ts = ts + INTERVAL '2 days' WHERE id BETWEEN :id AND :id + 200;
UPDATE orders SET qty = qty + 1 WHERE id BETWEEN :id AND :id + 100;
DELETE FROM orders WHERE id BETWEEN :id + 1000 AND :id + 1020;
SQL
echo "VACUUM orders;" >"$P/vac.sql"
cat >"$P/maint.sql" <<'SQL'
SELECT sift.col_promote('o_col');
SELECT sift.col_merge('o_col', random() < 0.3);
SQL
cat >"$P/read.sql" <<'SQL'
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT ordered_same();
COMMIT;
SQL
pgbench_bg primary 6 "$SECS" -f "$P/ins.sql@40" -f "$P/upd.sql@10" -f "$P/vac.sql@1" -f "$P/maint.sql@2"
PRIMARY_PID=$PGBENCH_PID
pgbench -n -h "$(dir standby)/sock" -p "$(port standby)" -U postgres -c 2 -j 2 -T "$SECS" -f "$P/read.sql" postgres >"$P/reader.log" 2>&1 &
READER=$!
wait "$PRIMARY_PID"
wait "$READER"
check "churn on the primary had no failed transaction" grep -q "number of failed transactions: 0 " "$P/pgbench.log"
check "ordered reads on the standby had no failed transaction" grep -q "number of failed transactions: 0 " "$P/reader.log"
catchup primary standby

ORD="SELECT string_agg(ts::text, ',') FROM (SELECT ts FROM orders ORDER BY ts DESC LIMIT 60 OFFSET 2500) s"
check "the ordered reads of the standby return its heap's" test "$(sql standby "SELECT ordered_same()")" == t
check "and the primary's" test "$(sql standby "$ORD")" == "$(sql primary "$ORD")"
sleep 2   # the standby's feedback no longer holds the horizon of its finished readers
sql primary "VACUUM (disable_page_skipping) orders" "SELECT sift.col_merge('o_col', true)" >/dev/null
catchup primary standby
check "after a VACUUM and a merge the standby's ordered reads return its heap's" test "$(sql standby "SELECT ordered_same()")" == t
check "the index verifies on the standby" test -z "$(sql standby "SELECT sift.col_verify('o_col', true)" 2>&1)"
SEGS="SELECT string_agg(gen || ':' || kind || ':' || format || ':' || coalesce(rows, 0), ' ' ORDER BY gen) FROM sift.col_segments('o_col') WHERE kind = 'immutable'"
check "standby immutable segments, with their formats, equal the primary's" test "$(sql standby "$SEGS")" == "$(sql primary "$SEGS")"

node_promote standby
for _ in $(seq 100); do [[ $(sql standby "SELECT pg_is_in_recovery()") == f ]] && break; sleep 0.1; done
sql standby "INSERT INTO orders (ts, qty) SELECT TIMESTAMP '2026-02-01' + i * INTERVAL '1 minute', i FROM generate_series(1, 500) i" \
	"SELECT sift.col_promote('o_col')" "SELECT sift.col_merge('o_col', true)" "VACUUM orders" >/dev/null
check "the promoted standby takes writes and its index verifies" test -z "$(sql standby "SELECT sift.col_verify('o_col', true)" 2>&1)"
check "and its ordered reads return its heap's" test "$(sql standby "SELECT ordered_same()")" == t
finish
