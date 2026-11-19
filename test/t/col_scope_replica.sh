#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup) of a sift_col index with a scope (test/t/col_index_replica.sh is the model): the primary inserts, updates and deletes facts, edits the pairs of the scope relation
# (inserts, deletes, moves an entity between keys: entities in 0 to 6 keys), vacuums, promotes and merges, while readers on the standby run REPEATABLE READ transactions that read one key through the scope path and
# through the plain join inside the same snapshot (they must agree; no failed transaction). After the replay the standby answers the scope forms (one key, a set counted once, per pair, top-N) as the primary's
# plain join does, the index verifies there, a REINDEX of the primary (the scope sidecars written again) replays to a standby that has them and reads as the join does, and the promoted standby takes fact and
# pair writes and its scope reads are the join. SIFT_COL_REPLICA_SECS sets the churn (15 s, 3 quick).
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_twohop.bash"
. "$(dirname "$0")/lib_side.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SECS=$(knob SIFT_COL_REPLICA_SECS 15 3)
plan 26
SAME_FN=$(cat <<'EOF'
CREATE FUNCTION scope_same(k int) RETURNS bool LANGUAGE plpgsql AS $$
	DECLARE a text; b text; c text; d text;
	BEGIN
		PERFORM set_config('sift.agg_force', 'on', true);
		PERFORM set_config('sift.enable_agg', 'on', true);
		SELECT count(*) || ':' || coalesce(sum(v), 0) || ':' || count(DISTINCT dim_a_id) INTO a FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, k);
		SELECT string_agg(id::text, ',' ORDER BY created_at DESC, id DESC) INTO c FROM (SELECT id, created_at FROM events e
			WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, k) ORDER BY created_at DESC, id DESC LIMIT 20) t;
		PERFORM set_config('sift.enable_agg', 'off', true);
		SELECT count(*) || ':' || coalesce(sum(v), 0) || ':' || count(DISTINCT e.dim_a_id) INTO b FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = k;
		SELECT string_agg(id::text, ',' ORDER BY created_at DESC, id DESC) INTO d FROM (SELECT id, created_at FROM events e
			WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = k) ORDER BY created_at DESC, id DESC LIMIT 20) t;
		IF a <> b OR c IS DISTINCT FROM d THEN RAISE EXCEPTION 'scope_same(%): scope % join %; top20 scope % join %', k, a, b, left(c, 60), left(d, 60); END IF;
		RETURN true;
	END $$
EOF
)
node_init scp 49 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off
$SIDE_CONF"
node_start scp || bail "primary start failed"
sql scp "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 40 FROM generate_series(1, 600) m, generate_series(0, 5) j WHERE j < m % 7 AND m NOT BETWEEN 100 AND 109" \
	"CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE events (id bigserial PRIMARY KEY, dim_a_id int, created_at timestamptz NOT NULL, v int NOT NULL, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events (dim_a_id, created_at, v, pad) SELECT CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 9999 ELSE 1 + (i * 7919) % 650 END,
		timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 60) FROM generate_series(1, 30000) i" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (write_buffer_limit = 64, key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)')" \
	"REINDEX INDEX events_col" \
	"VACUUM (FREEZE, ANALYZE) events, tenant_scope" \
	"$SAME_FN" \
	"CREATE FUNCTION scope_set_same(ks int8[]) RETURNS bool LANGUAGE plpgsql AS \$\$
	DECLARE a text; b text;
	BEGIN
		PERFORM set_config('sift.agg_force', 'on', true);
		PERFORM set_config('sift.enable_agg', 'on', true);
		SELECT count(*) || ':' || coalesce(sum(v), 0) INTO a FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, ks);
		PERFORM set_config('sift.enable_agg', 'off', true);
		SELECT count(*) || ':' || coalesce(sum(v), 0) INTO b FROM events e WHERE EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = e.dim_a_id AND s.tenant_id = ANY (ks));
		IF a <> b THEN RAISE EXCEPTION 'scope_set_same(%): scope % join %', ks, a, b; END IF;
		RETURN true;
	END \$\$" \
	>/dev/null || bail "setup failed"

node_clone scp scs 50 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start scs || bail "standby start failed"
ok "standby streaming from a base backup"

# a phase with no edit of the pairs since the build
sql scp "SELECT 1" >/dev/null
catchup scp scs
check "a fresh standby reads one key as the join does" test "$(sql scs "SELECT scope_same(5) AND scope_same(17) AND scope_same(40) AND scope_set_same('{2,9,31}')")" == t

P=$(dir scp)
cat >"$P/ins.sql" <<'EOF'
INSERT INTO events (dim_a_id, created_at, v, pad) SELECT 1 + (random() * 660)::int, timestamptz '2026-06-01' + i * interval '1 minute', i % 10, 'y' FROM generate_series(1, 20) i;
EOF
cat >"$P/upd.sql" <<'EOF'
\set id random(1, 30000)
UPDATE events SET v = (v + 3) % 10 WHERE id BETWEEN :id AND :id + 150;
UPDATE events SET dim_a_id = 1 + (dim_a_id * 7 + id) % 600 WHERE id BETWEEN :id + 200 AND :id + 260 AND dim_a_id IS NOT NULL;
DELETE FROM events WHERE id BETWEEN :id + 1000 AND :id + 1015;
EOF
cat >"$P/pairs.sql" <<'EOF'
\set m random(1, 600)
BEGIN;
SELECT pg_advisory_xact_lock(1);
DELETE FROM tenant_scope WHERE dim_a_id = :m AND tenant_id = (SELECT min(tenant_id) FROM tenant_scope WHERE dim_a_id = :m);
INSERT INTO tenant_scope SELECT :m, 1 + (random() * 39)::int ON CONFLICT DO NOTHING;
UPDATE tenant_scope t SET tenant_id = 1 + tenant_id % 40 WHERE dim_a_id = :m + 1 AND tenant_id = (SELECT max(tenant_id) FROM tenant_scope WHERE dim_a_id = :m + 1)
	AND NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = :m + 1 AND s.tenant_id = 1 + t.tenant_id % 40);
COMMIT;
EOF
echo "VACUUM events;" >"$P/vac.sql"
cat >"$P/maint.sql" <<'EOF'
SELECT sift.col_promote('events_col');
SELECT sift.col_merge('events_col', random() < 0.3);
EOF
cat >"$P/read.sql" <<'EOF'
\set k random(1, 41)
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT 1 / (scope_same(:k) AND scope_set_same(ARRAY[:k, 1 + :k % 40, 1 + (:k + 7) % 40]::int8[]))::int;
SELECT 1 / scope_same(1 + (:k * 3) % 40)::int;
COMMIT;
EOF
pgbench_bg scp 5 "$SECS" -f "$P/ins.sql@30" -f "$P/upd.sql@10" -f "$P/pairs.sql@20" -f "$P/vac.sql@1" -f "$P/maint.sql@2"
PRIMARY_PID=$PGBENCH_PID
pgbench -n -h "$(dir scs)/sock" -p "$(port scs)" -U postgres -c 2 -j 2 -T "$SECS" -f "$P/read.sql" postgres >"$P/reader.log" 2>&1 &
READER=$!
wait "$PRIMARY_PID"; PRC=$?
wait "$READER"; RRC=$?
clean_run() { ((${1} == 0)) && grep -q "number of failed transactions: 0 " "$2"; }   # an aborted client (an SQL error) ends pgbench with a status
check "churn of facts and pairs on the primary had no failed transaction" clean_run "$PRC" "$P/pgbench.log"
check "readers on the standby: the scope path and the join agree inside each snapshot, no failed transaction" clean_run "$RRC" "$P/reader.log"
catchup scp scs

ANS="SELECT string_agg(k || ':' || c, ' ' ORDER BY k) FROM (SELECT k, (SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = k) AS c FROM generate_series(1, 41) k) x"
check "the standby's join per pair is the primary's" test "$(sql scs "$ANS")" == "$(sql scp "$ANS")"
SC="SELECT bool_and(scope_same(k)) FROM generate_series(1, 41) k"
check "the standby reads every key through the scope path as its join does" test "$(sql scs "$SC")" == t
check "the same for sets of keys counted once" test "$(sql scs "SELECT scope_set_same('{2,9,31}') AND scope_set_same('{1,2,3,4,5,6}') AND scope_set_same('{40,41}') AND scope_set_same('{}')")" == t
check "the standby reads as the primary does" test "$(sql scs "$SC")" == "$(sql scp "$SC")"
check "the index verifies on the standby" test -z "$(sql scs "SELECT sift.col_verify('events_col', true)" 2>&1)"
# the scope sidecars written again, replayed: the standby has them and reads exactly
sql scp "REINDEX INDEX events_col" >/dev/null
catchup scp scs
check "after a REINDEX of the primary the standby has the sidecar of the entity segments" test "$(side_format scs events_col)" == 53
check "... and its reads are the join's" test "$(sql scs "$SC")" == t
check "the index verifies on the standby after the REINDEX" test -z "$(sql scs "SELECT sift.col_verify('events_col', true)" 2>&1)"

# a concurrent build while the writers of the primary go on, replayed by the standby: an index of a copy of the facts with the same scope is made with CREATE INDEX CONCURRENTLY and made again with REINDEX INDEX
# CONCURRENTLY beside inserts of facts and edits of the pairs; a session of the standby that took its REPEATABLE READ snapshot before each build reads, under it, as the join does (the primary holds back what it
# needs: hot_standby_feedback). Once the primary has vacuumed, both nodes read the keys through the second index as the join does and take the runs of its sidecars (written by the build, with no maintenance).
SAME_B=${SAME_FN//scope_same/scope_same_b}
SAME_B=${SAME_B//\'events_col\'/\'events_b_col\'}
SAME_B=${SAME_B//events e/events_b e}
sql scp "CREATE TABLE events_b (LIKE events INCLUDING DEFAULTS) WITH (autovacuum_enabled = false)" "ALTER TABLE events_b ADD PRIMARY KEY (id)" "INSERT INTO events_b SELECT * FROM events" "VACUUM (FREEZE, ANALYZE) events_b" "$SAME_B" >/dev/null || bail "the table of the second index failed"
catchup scp scs
CIC_B="CREATE INDEX CONCURRENTLY events_b_col ON events_b USING sift_col (id, dim_a_id, created_at, v) WITH (write_buffer_limit = 64, key_order = 'dim_a_id, created_at', key_cut = 'dim_a_id, created_at', scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)')"
RUN_B="SELECT bool_and(scope_same_b(k)) FROM generate_series(1, 41) k"
runs_b() { # NODE: the runs the reads of every key take through the second index
	local k n=0
	for k in $(seq 1 41); do n=$((n + $(side_runs "$1" "SELECT v, count(*), sum(id) FROM events_b e WHERE sift.in_scope('events_b_col'::regclass, 'tenant_id', e.dim_a_id, $k) GROUP BY v ORDER BY v"))); done
	echo "$n"
}
fact_inserts() { # a few batches of facts (they wait in the write buffer, in no segment)
	local i
	for i in $(seq 1 20); do sql scp "INSERT INTO events_b (dim_a_id, created_at, v, pad) SELECT 1 + (random() * 660)::int, timestamptz '2026-06-01' + g * interval '1 minute', g % 10, 'y' FROM generate_series(1, 20) g" >/dev/null; done
}
pair_edits() { # a few edits of the pairs of the first dim_b: the runs whose span has none keep their summaries, and the journal is far from a rewrite that would trim the stamps
	local i
	for i in $(seq 1 30); do sql scp "DELETE FROM tenant_scope WHERE dim_a_id = $((1 + i % 20)) AND tenant_id = $((40 - i % 9))" "INSERT INTO tenant_scope VALUES ($((1 + i % 20)), $((40 - (i + 3) % 9))) ON CONFLICT DO NOTHING" >/dev/null; done
}
concurrent_under_writers() { # WHAT STATEMENT
	side_rr_open scs
	local xmin
	xmin=$(side_rr_run "SELECT txid_snapshot_xmin(txid_current_snapshot())")
	wait_for "[[ \$(sql scp \"SELECT coalesce(xmin::text::bigint <= $xmin, false) FROM pg_replication_slots WHERE slot_name = 'scs'\") == t ]]" 60 || bail "$1: the primary does not hold the xmin of the snapshot"
	fact_inserts &
	INSERTS=$!
	pair_edits &
	EDITS=$!
	sql scp "$2" >"$P/build.out" 2>&1
	check "$1: the build ends beside inserts of facts and edits of the pairs" test $? == 0
	wait "$INSERTS" "$EDITS"
	catchup scp scs
	check "$1: the sidecars are on the standby before any maintenance of the index (format 53)" test "$(side_format scs events_b_col)" == 53
	rr=$(side_rr_run "SELECT bool_and(scope_same_b(k)) FROM generate_series(1, 41) k")
	check "$1: a REPEATABLE READ snapshot of the standby older than the build reads the join's rows" test "$rr" == t
	side_rr_close
	wait_for "[[ \$(sql scp \"SELECT coalesce(xmin::text::bigint > $xmin, true) FROM pg_replication_slots WHERE slot_name = 'scs'\") == t ]]" 60 || bail "$1: the primary holds the xmin of the closed snapshot"
	sql scp "VACUUM (INDEX_CLEANUP on) events_b" "SELECT pg_current_xact_id()" >/dev/null   # a transaction after the build: the standby's snapshots are newer than the index's row of pg_index (indcheckxmin)
	catchup scp scs
	local sp ss rp rs v
	sp=$(sql scp "$RUN_B"); ss=$(sql scs "$RUN_B"); rp=$(runs_b scp); rs=$(runs_b scs); v=$(sql scs "SELECT sift.col_verify('events_b_col', true)" 2>&1)
	check "$1: both nodes read the keys through the index as the join does and take its runs; the index verifies on the standby" test "$sp|$ss" == "t|t" -a "$rp" -gt 0 -a "$rs" -gt 0 -a -z "$v"
}
sql scp "ALTER SYSTEM SET sift.col_auto_merge = off" "SELECT pg_reload_conf()" >/dev/null   # the merge step would write the sidecars again behind the reads
concurrent_under_writers "CREATE INDEX CONCURRENTLY" "$CIC_B"
concurrent_under_writers "REINDEX INDEX CONCURRENTLY" "REINDEX INDEX CONCURRENTLY events_b_col"
sql scp "ALTER SYSTEM RESET sift.col_auto_merge" "SELECT pg_reload_conf()" >/dev/null

# the scope of two paths (lib_twohop.bash), made on the primary after the standby exists: edits of the three tables on the primary while readers of the standby compare the scope path and the join of the
# union inside each REPEATABLE READ snapshot
twohop_setup scp || bail "the fixtures of the scope of two paths failed"
catchup scp scs
FILES=($(twohop_files scp))
cat >"$P/hop_read.sql" <<'EOF'
\set k random(1, 17)
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT 1 / hop_same(:k)::int;
SELECT 1 / hop_same(1 + (:k * 3) % 16)::int;
COMMIT;
EOF
pgbench_bg scp 5 "$SECS" "${FILES[@]}"
PRIMARY_PID=$PGBENCH_PID
pgbench -n -h "$(dir scs)/sock" -p "$(port scs)" -U postgres -c 2 -j 2 -T "$SECS" -f "$P/hop_read.sql" postgres >"$P/hop_reader.log" 2>&1 &
READER=$!
wait "$PRIMARY_PID"; PRC=$?
wait "$READER"; RRC=$?
check "two paths: edits of the grants, the memberships and the facts on the primary had no failed transaction" clean_run "$PRC" "$P/pgbench.log"
check "two paths: readers on the standby, the scope path and the join of the union agree inside each snapshot, no failed transaction" clean_run "$RRC" "$P/hop_reader.log"
catchup scp scs
check "two paths: the standby reads as the join of the union, and as the primary does" test "$(twohop_reads scs)|$(sql scs "SELECT bool_and(hop_same(k)) FROM generate_series(1, 17) k")" == "same|t" \
	-a "$(sql scs "$TWOHOP_Q_PAIR")" == "$(sql scp "$TWOHOP_Q_PAIR")"
sql scp "REINDEX INDEX hop_col" >/dev/null
catchup scp scs
hop_after_reindex() {
	out="reads $(twohop_reads scs), verify: $(sql scs "SELECT sift.col_verify('hop_col', true)" "SELECT sift.col_verify('grants_by_member', true)" "SELECT sift.col_verify('grants_by_group', true)" "SELECT sift.col_verify('memberships_by_member', true)" 2>&1)"
	[[ $out == "reads same, verify: " ]]
}
check "two paths: after a REINDEX of the primary the reads of the standby are the join's and the indexes verify" hop_after_reindex

node_promote scs
for _ in $(seq 100); do [[ $(sql scs "SELECT pg_is_in_recovery()") == f ]] && break; sleep 0.1; done
sql scs "INSERT INTO events (dim_a_id, created_at, v, pad) SELECT 1 + i % 600, timestamptz '2026-07-01' + i * interval '1 minute', i % 10, 'z' FROM generate_series(1, 700) i" \
	"UPDATE tenant_scope SET tenant_id = 1 + tenant_id % 40 WHERE dim_a_id % 9 = 4 AND tenant_id < 40 AND NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = tenant_scope.dim_a_id AND s.tenant_id = 1 + tenant_scope.tenant_id % 40)" \
	"INSERT INTO tenant_scope VALUES (1, 41), (2, 41), (601, 41) ON CONFLICT DO NOTHING" \
	"SELECT sift.col_promote('events_col')" "SELECT sift.col_merge('events_col', true)" "VACUUM events" >/dev/null
check "the promoted standby takes writes: its index verifies and its scope reads are the join" \
	test -z "$(sql scs "SELECT sift.col_verify('events_col', true)" 2>&1)" -a "$(sql scs "$SC")" == t
sql scs "INSERT INTO grants (tenant_id, group_id) VALUES (3, 6), (4, 7) ON CONFLICT DO NOTHING" "UPDATE memberships SET deleted_at = now() WHERE group_id = 6 AND dim_a_id % 3 = 0" \
	"UPDATE grants SET deleted_at = now() WHERE dim_a_id % 11 = 0" "INSERT INTO hop_events SELECT 200000 + i, 1 + i % 210, now(), i % 10, 'p' FROM generate_series(1, 300) i" \
	"SELECT sift.col_promote('hop_col')" "SELECT sift.col_merge('hop_col', true)" >/dev/null
check "the promoted standby takes writes of the three relations of the scope of two paths: its reads are the join and its indexes verify" test "$(twohop_reads scs)" == same \
	-a -z "$(sql scs "SELECT sift.col_verify('hop_col', true)" "SELECT sift.col_verify('grants_by_member', true)" 2>&1)"
finish
