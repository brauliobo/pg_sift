#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup): inserts, updates, deletes, VACUUM, promotion and merge of a sift_col index on the primary
# replay to an index that verifies on the hot standby and answers the column source exactly as the primary does. Readers on the standby
# run all the while (with hot_standby_feedback on none may fail). The weight leaf holds numbers among strings, booleans, objects, arrays and JSON
# nulls (overflow values with their JSON types in the chunks) and some documents lack it: jsonb_typeof filters (=, <>, 'null', IS NULL), a group key and FILTERs return the same on the standby, the primary and the heap.
# The standby is then promoted and takes writes. key_cut is off: the rewrite chooses it again from the data (tenant_id here, 13 units), and the units of a cut
# index are not answered net of exceptions (an IS NULL on the leaf that is mostly NULL scans them), which the check of the merged segment asks for.
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_col.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SECS=$(knob SIFT_COL_REPLICA_SECS 15 3)
plan 22
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"${COL_WEIGHT_FUNCS[@]}" \
	"CREATE TABLE orders (id bigserial PRIMARY KEY, tenant_id int, status text, qty bigint, gone bigint, doc jsonb)" \
	"CREATE FUNCTION kept_same(tbl regclass) RETURNS bool LANGUAGE plpgsql AS \$\$
	DECLARE q text = format('SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (WITH ev AS (SELECT k, grp, v FROM %s WHERE v < 6)
			SELECT c.grp, c.n, s.total FROM (SELECT grp, count(*) AS n FROM ev GROUP BY grp) c JOIN (SELECT grp, sum(v) AS total FROM ev GROUP BY grp) s USING (grp)) x', tbl);
		a text; b text;
	BEGIN
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE q INTO a;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE q INTO b;
		RETURN a = b;
	END \$\$" \
	"CREATE FUNCTION ordered_same(tbl regclass) RETURNS bool LANGUAGE plpgsql AS \$\$
	DECLARE q text = format('SELECT md5(string_agg(id::text, '','' ORDER BY ord)) FROM (SELECT row_number() OVER () AS ord, id FROM (SELECT id FROM %s ORDER BY k, id LIMIT 300) s) s', tbl);
		a text; b text;
	BEGIN
		PERFORM set_config('sift.enable_agg', 'on', true);
		PERFORM set_config('sift.enable_col_scan', 'on', true);
		EXECUTE q INTO a;
		PERFORM set_config('sift.enable_agg', 'off', true);
		PERFORM set_config('sift.enable_col_scan', 'off', true);
		EXECUTE q INTO b;
		RETURN a = b;
	END \$\$" \
	"INSERT INTO orders (tenant_id, status, qty, gone, doc) SELECT i % 50, (ARRAY['new','paid','void'])[1 + i % 3], i, CASE WHEN i % 500 = 0 THEN i END,
		jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) || weight_doc(i) FROM generate_series(1, 20000) i" \
	"CREATE INDEX o_col ON orders USING sift_col (tenant_id, status, qty, gone, doc) WITH (key_cut = off, write_buffer_limit = 64, paths = 'doc.items[]{category text, qty int4}, doc.weight float8',
	preagg = 'cube(tenant_id: qty; status: qty) rollup(doc.items[].category: doc.items[].qty)', hll_columns = 'tenant_id, status, qty')" \
	>/dev/null || bail "setup failed"
# kept_o has the global order of k: a session that keeps the rows of the write buffer keeps their order too
for T in kept_a kept_b kept_c kept_o; do
	OPTS=; [[ $T == kept_o ]] && OPTS="WITH (order_columns = 'k', order_array_columns = 'k')"
	sql primary "CREATE TABLE $T (id int PRIMARY KEY, k int NOT NULL, grp int NOT NULL, v int NOT NULL, doc jsonb)" \
		"INSERT INTO $T SELECT i, i % 3000, i % 40, i % 17, jsonb_build_object('a', i % 9) FROM generate_series(1, 40000) i" \
		"CREATE INDEX ${T}_col ON $T USING sift_col (id, k, grp, v, doc) $OPTS" "VACUUM (FREEZE, ANALYZE) $T" >/dev/null || bail "setup of $T failed"
done

node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

P=$(dir primary)
cat >"$P/ins.sql" <<'EOF'
INSERT INTO orders (tenant_id, status, qty, doc) SELECT (random() * 50)::int, 'paid', i,
	jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) || weight_doc((random() * 1000)::int) FROM generate_series(1, 20) i;
EOF
cat >"$P/upd.sql" <<'EOF'
\set id random(1, 100000)
UPDATE orders SET tenant_id = coalesce(tenant_id, 0) + 1 WHERE id BETWEEN :id AND :id + 200;
UPDATE orders SET gone = id WHERE id = :id;
DELETE FROM orders WHERE id BETWEEN :id + 1000 AND :id + 1020;
EOF
echo "VACUUM orders;" >"$P/vac.sql"
cat >"$P/maint.sql" <<'EOF'
SELECT sift.col_promote('o_col');
SELECT sift.col_merge('o_col', random() < 0.3);
EOF
cat >"$P/read.sql" <<'EOF'
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM sift._test_col_tids('o_col', '0:lt:10;1:eq:paid');
SELECT typeof_same();
SELECT gone_same();
COMMIT;
EOF
pgbench_bg primary 6 "$SECS" -f "$P/ins.sql@40" -f "$P/upd.sql@10" -f "$P/vac.sql@1" -f "$P/maint.sql@2"
PRIMARY_PID=$PGBENCH_PID
pgbench -n -h "$(dir standby)/sock" -p "$(port standby)" -U postgres -c 2 -j 2 -T "$SECS" -f "$P/read.sql" postgres >"$P/reader.log" 2>&1 &
READER=$!
wait "$PRIMARY_PID"
wait "$READER"
check "churn on the primary had no failed transaction" grep -q "number of failed transactions: 0 " "$P/pgbench.log"
check "readers on the standby had no failed transaction" grep -q "number of failed transactions: 0 " "$P/reader.log"
catchup primary standby

CHECKS="SELECT (SELECT count(*) FROM sift._test_col_tids('o_col', '')) || ' ' || (SELECT count(*) FROM sift._test_col_tids('o_col', '0:lt:10;1:eq:paid'))
	|| ' ' || (SELECT count(*) FROM orders) || ' ' || (SELECT count(*) FROM orders WHERE tenant_id < 10 AND status = 'paid')"
check "standby answers the column source as the primary does" test "$(sql standby "$CHECKS")" == "$(sql primary "$CHECKS")"
check "the standby computes the groups of the elements from the element cubes as the primary does" \
	test "$(elem_groups standby on)" == "$(elem_groups primary off)"
# The units of the merged segment are answered net of exceptions only when the pages of the table are all-visible. VACUUM leaves a page that holds dead items out of it, and with
# few such pages (under 2%) it bypasses the index cleanup of its own accord: index_cleanup on makes it remove them. A snapshot of the primary older than the churn (a session that
# has not closed, a maintenance worker) keeps the dead tuples and the newest pages out of it as well: the VACUUM repeats until it is past it, and the log names the holder
held=
holders() { sql primary "SELECT string_agg(backend_type || ' xmin ' || backend_xmin || ' since ' || coalesce(xact_start::text, '-'), '; ') FROM pg_stat_activity WHERE backend_xmin IS NOT NULL AND pid <> pg_backend_pid()"; }
vacuumed() {
	vac=$(sql primary "SET client_min_messages = info" "VACUUM (disable_page_skipping, index_cleanup on, verbose) orders" 2>&1)
	[[ $(sql primary "SELECT relallvisible = relpages FROM pg_class WHERE oid = 'orders'::regclass") == t ]] || { held=${held:-$(holders)}; return 1; }
}
wait_for vacuumed 120 || bail "the VACUUM of the primary leaves pages that are not all-visible; held by: $held; $(grep -E 'tuples:|index scan|visibility map' <<<"$vac" | tr '\n' ' ')"
[[ -z $held ]] || diag "the first VACUUM left pages that are not all-visible; held by: $held"
sql primary "SELECT sift.col_merge('o_col', true)" >/dev/null
catchup primary standby
GONE="SELECT status, count(*), sum(qty) FROM orders WHERE gone IS NULL GROUP BY 1 ORDER BY 1"
check "the standby answers the soft-delete filter as the primary and the heap do" \
	test "$(sql standby "SET seq_page_cost = 10000" "$GONE")" == "$(sql primary "SET sift.enable_agg = off" "$GONE")" -a "$(sql standby "SELECT gone_same()")" == t
out=$(sql standby "SET seq_page_cost = 10000" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $GONE" \
	"SELECT string_agg(kind || ':' || rows || ':' || units || ':' || dirty || ':' || dead_rows, ' ') FROM sift.col_segments('o_col')")
out="$(grep -E 'tuples:' <<<"$vac")"$'\n'"$out"
check "the standby answers units of the merged segment from the exceptions" grep -qE "[1-9][0-9]* net of exceptions" <<<"$out"
out=
APPROX="SELECT sift.approx_count_distinct(qty) || ' ' || sift.approx_count_distinct(status) || ' ' || sift.approx_count_distinct(tenant_id) FROM orders"
check "the standby merges the sketches of its units to the number the primary's heap gives" \
	test "$(sql standby "SET sift.agg_force = on" "$APPROX")" == "$(sql primary "SET sift.enable_agg = off" "$APPROX")"
check "the index verifies on the standby" test -z "$(sql standby "SELECT sift.col_verify('o_col', true)" 2>&1)"
TYPED="SELECT count(*) || ' ' || sum((doc->>'weight')::float8) FROM orders WHERE jsonb_typeof(doc->'weight') = 'number'"
check "a type filter plans as an aggregate scan on the standby" \
	test "$(sql standby "SET seq_page_cost = 10000" "EXPLAIN (COSTS OFF) $TYPED" | grep -c SiftAgg)" == 1
check "the standby's type filter returns what its heap returns" test "$(sql standby "SELECT typeof_same()")" == t
check "and what the primary's returns" \
	test "$(sql standby "SET seq_page_cost = 10000" "$TYPED")" == "$(sql primary "SET seq_page_cost = 10000" "$TYPED")"
SEGS="SELECT string_agg(gen || ':' || kind || ':' || coalesce(rows, 0), ' ' ORDER BY gen) FROM sift.col_segments('o_col') WHERE kind = 'immutable'"
check "standby immutable segments equal the primary's" test "$(sql standby "$SEGS")" == "$(sql primary "$SEGS")"

# A session of the standby keeps the rows of the write buffer between statements. After updates and deletes the primary's VACUUM flags the records of the tuples it removes
# (replayed here) and frees their addresses: the session's next read must be the heap's, not what it kept
for T in kept_a kept_b kept_c kept_o; do
	sql primary "SET sift.async_maintenance = off" "UPDATE $T SET doc = NULL WHERE id % 11 = 0" "UPDATE $T SET grp = (grp + 1) % 40 WHERE id % 17 = 0" "UPDATE $T SET v = v + 3 WHERE id % 13 = 0" \
		"DELETE FROM $T WHERE id % 19 = 0" >/dev/null
done
catchup primary standby
check "the standby reads the rows of the write buffer through the global order of k" \
	grep -q "Global Order: true" <<<"$(sql standby "SET max_parallel_workers_per_gather = 0" "SET sift.agg_force = on" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT id FROM kept_o ORDER BY k, id LIMIT 300")"
# the version of the directory is what tells a reader that keeps batches that the VACUUM flagged records: it must replay with the flags, before the addresses are reused
VERS="SELECT string_agg(sift._test_col_dir_version(i::regclass)::text, ' ' ORDER BY i) FROM unnest(ARRAY['kept_a_col', 'kept_b_col', 'kept_c_col', 'kept_o_col']) i"
VBEFORE=$(sql standby "$VERS")
rm -f "$P/kept.go"
cat >"$P/kept.sql" <<EOF
SET max_parallel_workers_per_gather = 0;
SELECT kept_same('kept_a') AND kept_same('kept_b') AND kept_same('kept_c') AND ordered_same('kept_o');
\! until [ -f $P/kept.go ]; do sleep 0.1; done
SELECT kept_same('kept_a') AND kept_same('kept_b') AND kept_same('kept_c') AND ordered_same('kept_o');
EOF
psql -X -At -q -h "$(dir standby)/sock" -p "$(port standby)" -U postgres -d postgres -f "$P/kept.sql" >"$P/kept.log" 2>&1 &
KEPT=$!
for _ in $(seq 100); do [[ -s $P/kept.log ]] && break; sleep 0.1; done
sleep 2   # the standby's feedback holds the horizon below the deletes until it reports its session done, and a VACUUM below it removes nothing
sql primary "SET sift.async_maintenance = off" "VACUUM kept_a, kept_b, kept_c, kept_o" >/dev/null
catchup primary standby
VAFTER=$(sql standby "$VERS")
touch "$P/kept.go"
wait "$KEPT"
check "the VACUUM of the primary moved the version of the directory of each index on the standby" \
	test "$(paste -d' ' <(tr ' ' '\n' <<<"$VBEFORE") <(tr ' ' '\n' <<<"$VAFTER") | awk '$1 >= $2' | wc -l)" == 0
check "... and the standby has the version of each index as the primary has it" test "$VAFTER" == "$(sql primary "$VERS")"
check "a session of the standby that read the write buffer reads the heap as it is after the VACUUM" test "$(tr '\n' ' ' <"$P/kept.log")" == "t t "

# deletes alone (no row for the write buffer, so no rewrite) and a VACUUM leave dead rows in the units of a table of its own, and the pre-aggregates net of them; their pages replay on the standby like any others net of them, whose pages replay on the standby like any others
sql primary "SET sift.async_maintenance = off" "CREATE TABLE net_d (id int PRIMARY KEY, k int NOT NULL, grp int NOT NULL, v int NOT NULL, doc jsonb)" \
	"INSERT INTO net_d SELECT i, i % 3000, i % 40, i % 17, jsonb_build_object('a', i % 9) FROM generate_series(1, 40000) i" "CREATE INDEX net_d_col ON net_d USING sift_col (id, k, grp, v, doc)" "VACUUM (FREEZE, ANALYZE) net_d" \
	"DELETE FROM net_d WHERE id % 23 = 1" >/dev/null
sleep 2   # the standby's feedback holds the horizon below the delete until it reports again
sql primary "SET sift.async_maintenance = off" "VACUUM (disable_page_skipping) net_d" >/dev/null
catchup primary standby
MINMAX="SELECT grp, min(v), max(v), count(*) FROM net_d GROUP BY grp ORDER BY grp"
check "the standby answers the minima and maxima of a table net of the dead rows its primary's VACUUM left" \
	grep -qE "[1-9][0-9]* net of dead rows" <<<"$(sql standby "SET seq_page_cost = 10000" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $MINMAX")"
check "... and gives the groups of the heap" test "$(sql standby "SET seq_page_cost = 10000" "$MINMAX" | "$REPO/scripts/sha256.sh")" == "$(sql primary "SET sift.enable_agg = off" "$MINMAX" | "$REPO/scripts/sha256.sh")"
check "... and the index verifies there" test -z "$(sql standby "SELECT sift.col_verify('net_d_col', true)" 2>&1)"

node_promote standby
for _ in $(seq 100); do [[ $(sql standby "SELECT pg_is_in_recovery()") == f ]] && break; sleep 0.1; done
sql standby "INSERT INTO orders (tenant_id, status, qty, doc) SELECT 1, 'paid', i, '{\"items\": [{\"category\": \"c1\", \"qty\": 1}]}' FROM generate_series(1, 500) i" \
	"SELECT sift.col_promote('o_col')" "SELECT sift.col_merge('o_col', true)" "VACUUM orders" >/dev/null
check "the promoted standby takes writes and its index verifies" test -z "$(sql standby "SELECT sift.col_verify('o_col', true)" 2>&1)"
check "the promoted standby's index holds what its heap holds" \
	test "$(sql standby "SELECT (SELECT count(*) FROM sift._test_col_tids('o_col', '')) = (SELECT count(*) FROM orders) AND typeof_same()")" == t
finish
