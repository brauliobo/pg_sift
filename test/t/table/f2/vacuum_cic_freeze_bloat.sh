#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The VACUUM, the validation of CREATE INDEX CONCURRENTLY, ANALYZE and the DDL of a sift_table keep what they promise to the rest of the server:
#   cic     the validation reads every window of virtual blocks (a row a concurrent insert added past the first window is in the index, and a duplicate of it is refused)
#   freeze  pg_class.relfrozenxid follows the oldest transaction of the micro-slab and of the pending deletes, and never stays at a frozen part
#   analyze the dead rows the sample finds are reported, every table of one ANALYZE is sampled from its first granule, and reltuples holds when the granules outnumber the blocks
#   horizon the VACUUM holds the vacuum-read lock of the manifest, and writers racing it never quarantine the table
#   sweep   the orphan sweep reclaims the extents of an aborted wide value of a text table and names those a micro-slab holds
#   ddl     ALTER TABLE SET ACCESS METHOD forgets the catalog rows of the table, and RENAME COLUMN follows the column of its TTL
. "${SIFT_REPO:?}/test/t/lib/common.bash"

done_testing
node_init vc 3 "shared_preload_libraries = 'pg_sift'
autovacuum = off
sift.async_maintenance = off
shared_buffers = 64MB
max_wal_size = 1GB
max_parallel_workers_per_gather = 0
${SIFT_TESTING:+sift.test_create_format1 = off}"
node_start vc || bail "start failed"
sql vc "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
D=$(dir vc)
relfrozen() { sql vc "SELECT relfrozenxid::text::bigint FROM pg_class WHERE relname = '$1'"; }
xid_of() { sed -n 's/^xid //p' "$D/$1.out" | tail -n 1; }

# ---- cic: windows of 225 virtual blocks (the memory floor of the validation) over a table of 690, one window ahead of the rows of the insert that races the build
cic_race() {
	local ddl=$1 name=$2 stmt=$3 pid
	sess_open vc ga
	sess_open vc gc
	sess_say ga "BEGIN; LOCK TABLE cic IN ROW EXCLUSIVE MODE; \\echo held"
	wait_for "grep -q '^held\$' $D/ga.out" 60
	sql vc "SET maintenance_work_mem = '64kB'" "$ddl" >"$D/b.out" 2>&1 &
	pid=$!
	wait_for "[[ \$(sql vc \"SELECT count(*) FROM pg_index WHERE indexrelid = to_regclass('$name')\") == 1 ]]" 60
	sess_say gc "BEGIN; $stmt; \\echo inserted"
	wait_for "grep -q '^inserted\$' $D/gc.out" 60
	sess_say ga "COMMIT;"
	wait_for "[[ \$(sql vc \"SELECT indisready FROM pg_index WHERE indexrelid = to_regclass('$name')\") == t ]]" 60
	sess_say gc "COMMIT;"
	wait "$pid"
	CIC_RC=$?
	sess_close ga
	sess_close gc
}
via_index() {
	sql vc "SET sift.enable_agg = off" "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SET enable_indexonlyscan = off" "SELECT count(*) FROM cic WHERE v >= 0" | tail -n 1
}

sql vc "CREATE TABLE cic (id int, v int) USING sift_table" "INSERT INTO cic SELECT g, g FROM generate_series(1, 200000) g" >/dev/null || bail "cic load failed"
vblocks=$(sql vc "SELECT max((ctid::text::point)[0])::int FROM cic")
check "cic: the table spans at least three windows ($vblocks virtual blocks)" test "$vblocks" -ge 675
cic_race "CREATE INDEX CONCURRENTLY cic_i ON cic (v)" cic_i "INSERT INTO cic SELECT 200000 + g, 200000 + g FROM generate_series(1, 3000) g"
out="rc $CIC_RC: $(<"$D/b.out")"
check "cic: the build ends well" test "$CIC_RC" -eq 0
out="plan: $(sql vc "SET sift.enable_agg = off" "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SET enable_indexonlyscan = off" "EXPLAIN (COSTS OFF) SELECT count(*) FROM cic WHERE v >= 0")"
check "cic: a scan reads the new index" grep -q cic_i <<<"$out"
out="index $(via_index) of $(sql vc "SELECT count(*) FROM cic")"
check "cic: the index holds the rows a concurrent insert added past the first window" test "$(via_index)" -eq 203000
cic_race "CREATE UNIQUE INDEX CONCURRENTLY cic_u ON cic (id)" cic_u "INSERT INTO cic VALUES (5, 5)"
out="rc $CIC_RC: $(<"$D/b.out")"
refused() { [[ $CIC_RC -ne 0 ]] && grep -q 'duplicate key value violates unique constraint "cic_u"' "$D/b.out"; }
check "cic: a duplicate of a row of the first window, inserted past it, is refused" refused

# ---- freeze: relfrozenxid and the transactions the micro-slab and the pending deletes still hold
sql vc "CREATE TABLE fz (id int, v int) USING sift_table" >/dev/null || bail "fz failed"
before=$(relfrozen fz)
sess_open vc fa
sess_say fa "BEGIN; INSERT INTO fz VALUES (-1, -1); SELECT 'xid ' || pg_current_xact_id();"
wait_for "grep -q '^xid ' $D/fa.out" 60
sql vc "INSERT INTO fz SELECT g, g FROM generate_series(1, 1000) g" >/dev/null
sess_say fa "COMMIT;"
sess_close fa
xa=$(xid_of fa)
sql vc "VACUUM fz" >/dev/null
after=$(relfrozen fz)
out="before $before after $after xid of the micro-slab $xa"
check "freeze: relfrozenxid advances past a table with a frozen part and a micro-slab" test "$after" -gt "$before"
check "freeze: relfrozenxid does not pass the transaction the micro-slab holds" test "$after" -le "$xa"

sql vc "CREATE TABLE fd (id int, v int) USING sift_table" "INSERT INTO fd SELECT g, g FROM generate_series(1, 1000) g" "VACUUM FREEZE fd" >/dev/null || bail "fd failed"
for _ in 1 2 3; do sql vc "SELECT pg_current_xact_id()" >/dev/null; done
before=$(relfrozen fd)
sess_open vc fb
sess_say fb "BEGIN; SELECT 'xid ' || pg_current_xact_id();"
wait_for "grep -q '^xid ' $D/fb.out" 60
sql vc "DELETE FROM fd WHERE id = 1" "VACUUM fd" >/dev/null
after=$(relfrozen fd)
xb=$(xid_of fb)
sess_say fb "COMMIT;"
sess_close fb
out="before $before after $after oldest open transaction $xb"
check "freeze: relfrozenxid advances past a frozen part with a delete not settled" test "$after" -gt "$before"
check "freeze: relfrozenxid does not pass the oldest open transaction" test "$after" -le "$xb"

# ---- analyze
sql vc "CREATE TABLE an (id int, v int) USING sift_table" "INSERT INTO an SELECT g, g FROM generate_series(1, 100000) g" \
	"DELETE FROM an WHERE id % 7 = 0" "UPDATE an SET v = v + 1 WHERE id % 11 = 0" "SELECT pg_stat_force_next_flush()" "ANALYZE an" "SELECT pg_stat_force_next_flush()" >/dev/null || bail "an failed"
dead=$(sql vc "SELECT n_dead_tup FROM pg_stat_user_tables WHERE relname = 'an'")
out="n_dead_tup $dead"
check "analyze: the dead rows of a churned table are reported to the statistics" test "$dead" -gt 0

sql vc "CREATE TABLE ma (id int) USING sift_table" "CREATE TABLE mb (id int) USING sift_table" "CREATE TABLE mc (id int) USING sift_table" >/dev/null || bail "ma failed"
for t in ma mb mc; do sql vc "INSERT INTO $t SELECT g FROM generate_series(1, 1000) g" >/dev/null; done
sql vc "ANALYZE ma, mb, mc" >/dev/null
out=$(sql vc "SELECT relname || ' ' || reltuples FROM pg_class WHERE relname IN ('ma', 'mb', 'mc') ORDER BY 1")
check "analyze: every table of one ANALYZE is sampled from its first granule" test "$(tr '\n' ',' <<<"$out")" = "ma 1000,mb 1000,mc 1000,"

sql vc "CREATE TABLE bz (a int, b int, pad text) USING sift_table" "INSERT INTO bz SELECT g % 3, 7, 'x' FROM generate_series(1, 1000000) g" "VACUUM bz" "ANALYZE bz" >/dev/null || bail "bz failed"
granules=$(sql vc "SELECT sum(granules) FROM sift.parts('bz')")
blocks=$(sql vc "SELECT pg_relation_size('bz') / 8192")
est=$(sql vc "SELECT reltuples::bigint FROM pg_class WHERE relname = 'bz'")
out="granules $granules blocks $blocks reltuples $est of 1000000"
check "analyze: the table has more granules than blocks" test "$granules" -gt "$blocks"
check "analyze: reltuples is within 15% of the rows when the granules outnumber the blocks" test "$est" -ge 850000 -a "$est" -le 1150000

# ---- horizon: the VACUUM holds the vacuum-read lock of the manifest while it reads it
cat >"$D/watch.sql" <<'SQL'
DO $$
DECLARE hits int := 0;
BEGIN
	RAISE WARNING 'watching';
	FOR i IN 1..1500 LOOP
		IF EXISTS (SELECT 1 FROM pg_locks WHERE locktype = 'page' AND mode = 'ShareLock' AND relation = 'bz'::regclass AND page = -2044) THEN hits := hits + 1; END IF;
		EXIT WHEN hits > 0;
		PERFORM pg_sleep(0.001);
	END LOOP;
	RAISE WARNING 'hits %', hits;
END $$;
SQL
sql vc "DELETE FROM bz WHERE a = 0" >/dev/null
sql vc <"$D/watch.sql" >"$D/watch.out" 2>&1 &
pid=$!
wait_for "grep -q watching $D/watch.out" 60
sql vc "VACUUM bz" >/dev/null
wait "$pid"
out=$(<"$D/watch.out")
check "horizon: a VACUUM holds the vacuum-read lock of the manifest" grep -q 'hits 1' "$D/watch.out"

sql vc "CREATE TABLE cw (w int, id int, v int) USING sift_table" >/dev/null || bail "cw failed"
writer() {
	local w=$1 i
	for i in $(seq 1 25); do
		sql vc "INSERT INTO cw SELECT $w, $i * 1000 + g, g FROM generate_series(1, 400) g" "UPDATE cw SET v = v + 1 WHERE w = $w AND id % 50 = 0" "DELETE FROM cw WHERE w = $w AND id % 50 = 1" || return 1
	done
}
writers=()
for w in 1 2 3; do
	writer "$w" &
	writers+=($!)
done
for _ in 1 2 3 4 5 6; do sql vc "VACUUM cw" >/dev/null; done
rc=0
for pid in "${writers[@]}"; do wait "$pid" || rc=1; done
sql vc "VACUUM cw" >/dev/null
out=$(grep -ciE 'quarantin|SIFT_CORRUPT|is damaged' "$D/postgres.log")
check "horizon: writers racing VACUUM never quarantine the table ($out lines of the log)" test "$rc" -eq 0 -a "$out" -eq 0
want=$((3 * 25 * (400 - 8)))
out="rows $(sql vc "SELECT count(*) FROM cw") of $want"
check "horizon: no row is lost or doubled" test "$(sql vc "SELECT count(*) FROM cw")" -eq "$want"

# ---- sweep: the extents of an aborted wide value of a text table, with a wide value a micro-slab holds
wide() { echo "(SELECT string_agg(md5(($1 * 1000 + i)::text), '') FROM generate_series(1, ${2:-600}) i)"; }
sql vc "CREATE TABLE sw (id int, t text, j jsonb) USING sift_table" "INSERT INTO sw SELECT g, $(wide g), jsonb_build_object('k', g) FROM generate_series(1, 200) g" "INSERT INTO sw SELECT 1000 + g, $(wide g), '{}' FROM generate_series(1, 200) g" >/dev/null || bail "sw failed"
used() { sql vc "SELECT used_blocks FROM sift.space('$1')"; }
wanted() { sql vc "SELECT count(*) || '/' || md5(string_agg(md5(t), ',' ORDER BY id)) FROM (SELECT id, (SELECT string_agg(md5((k * 1000 + i)::text), '') FROM generate_series(1, n) i) t FROM (SELECT g id, g k, 600 n FROM generate_series(1, 200) g UNION ALL SELECT 1000 + g, g, 600 FROM generate_series(1, 200) g UNION ALL SELECT 100000 + g, g, 6000 FROM generate_series(1, 10) g) r) x"; }
digest() { sql vc "SELECT count(*) || '/' || md5(string_agg(md5(t), ',' ORDER BY id)) FROM $1 WHERE id < 300000"; }
base=$(used sw)
sql vc "BEGIN" "INSERT INTO sw SELECT 200000 + g, $(wide g), '{}' FROM generate_series(1, 250) g" "ROLLBACK" >/dev/null
leaked=$(used sw)
for g in $(seq 1 10); do sql vc "INSERT INTO sw SELECT 100000 + $g, $(wide $g 6000), '{}'" >/dev/null || bail "sw slab failed"; done
live=$((base + $(used sw) - leaked))
sql vc "VACUUM sw" >/dev/null
after=$(used sw)
out="live $live, after the abort $leaked, after VACUUM $after"
check "sweep: an aborted insert of wide values leaks extents" test "$leaked" -ge $((base + 300))
check "sweep: VACUUM gives the extents of the aborted wide values back" test "$after" -le $((live + 16))
check "sweep: it keeps the extents of the wide values a micro-slab holds" test "$after" -ge $((live - 16))
sql vc "INSERT INTO sw SELECT 300000 + g, $(wide g), '{}' FROM generate_series(1, 250) g" >/dev/null
want=$(wanted)
out="want $want, read $(digest sw)"
check "sweep: the wide values of the table, the micro-slab's among them, survive the reuse of the extents the sweep gave back" test "$(digest sw)" = "$want"

# with a standby streaming the orphans enter the quarantine of the allocator and leave it with its horizon
sql vc "CREATE TABLE sx (id int, t text) USING sift_table" "INSERT INTO sx SELECT g, $(wide g) FROM generate_series(1, 400) g" >/dev/null || bail "sx failed"
live=$(used sx)
sql vc "BEGIN" "INSERT INTO sx SELECT 200000 + g, $(wide g) FROM generate_series(1, 250) g" "ROLLBACK" >/dev/null
node_clone vc sb 4 "hot_standby_feedback = on"
node_start sb || bail "standby start failed"
check "sweep: the standby catches up" catchup vc sb
sql vc "VACUUM sx" >/dev/null
reclaimed() { sql vc "VACUUM sx" >/dev/null; [[ $(used sx) -le $((live + 16)) ]]; }
out="live $live, now $(used sx)"
check "sweep: with a standby streaming VACUUM gives the extents of the aborted wide values back" wait_for reclaimed 60
check "sweep: the standby replays the sweep and reads every row" test "$(catchup vc sb && sql sb "SELECT count(*) FROM sx")" -eq 400

# ---- ddl
TIER=$D/tier
mkdir -p "$TIER"
sql vc "CREATE TABLE dd (ts timestamptz NOT NULL, v int, doc jsonb) USING sift_table" "INSERT INTO dd SELECT now() - g * interval '1 minute', g, '{\"k\": 1}' FROM generate_series(1, 2000) g" \
	"SELECT sift.table_set_option('dd', 'key_order', 'v')" "SELECT sift.table_set_paths('dd', 'doc', 'doc.k int4')" "SELECT sift.create_projection('dd', 'by_v', 'ORDER BY v')" \
	"SELECT sift.set_tier_policy('dd', 'file://$TIER', '45 days')" "SELECT sift.set_ttl('dd', 'ts', '30 days')" >/dev/null || bail "dd failed"
rows_of() {
	sql vc "SELECT (SELECT count(*) FROM sift.table_option_values WHERE rel::oid = 'dd'::regclass::oid) || ' ' || (SELECT count(*) FROM sift.table_paths WHERE relid::oid = 'dd'::regclass::oid)
		|| ' ' || (SELECT count(*) FROM sift.projection_defs WHERE rel::oid = 'dd'::regclass::oid) || ' ' || (SELECT count(*) FROM sift.tier_policies WHERE relid::oid = 'dd'::regclass::oid)
		|| ' ' || (SELECT count(*) FROM sift.ttl_policies WHERE relid::oid = 'dd'::regclass::oid)"
}
out="rows of the options, paths, projections, tiers and TTL: $(rows_of)"
check "ddl: the table has a row in each of the five catalogs" test "$(rows_of)" = "1 1 1 1 1"
sql vc "ALTER TABLE dd RENAME COLUMN ts TO created_ts" >/dev/null
out=$(sql vc "SELECT col FROM sift.ttl_policies WHERE relid::oid = 'dd'::regclass::oid")
check "ddl: RENAME COLUMN renames the column of the TTL" test "$out" = created_ts
sql vc "ALTER TABLE dd SET ACCESS METHOD heap" >/dev/null
out="rows of the options, paths, projections, tiers and TTL: $(rows_of)"
check "ddl: SET ACCESS METHOD heap removes the five rows of the table" test "$(rows_of)" = "0 0 0 0 0"

diag "the script took ${SECONDS}s"
finish
