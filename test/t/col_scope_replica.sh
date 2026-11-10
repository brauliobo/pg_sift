#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup) of a sift_col index with a scope (test/t/col_index_replica.sh is the model): the primary inserts, updates and deletes facts, edits the pairs of the scope relation
# (inserts, deletes, moves an entity between keys: entities in 0 to 6 keys), vacuums, promotes and merges, while readers on the standby run REPEATABLE READ transactions that read one key through the scope path and
# through the plain join inside the same snapshot (they must agree; no failed transaction). After the replay the standby answers the scope forms (one key, a set counted once, per pair, top-N) as the primary's
# plain join does, the index verifies there, a REINDEX of the primary (the scope segments written again) replays to a standby that reads them (stored where the reader may, exact always), and the promoted
# standby takes fact and pair writes and its scope reads are the join. The reads are under the real verdict of the reader; the proof-of-concept switch is used only in the phase where no pair has been edited
# since the build (a phase the script knows). SIFT_COL_REPLICA_SECS sets the churn (15 s, 3 quick).
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SECS=$(knob SIFT_COL_REPLICA_SECS 15 3)
plan 14
node_init scp 49 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off"
node_start scp || bail "primary start failed"
sql scp "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false)" \
	"INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 40 FROM generate_series(1, 600) m, generate_series(0, 5) j WHERE j < m % 7 AND m NOT BETWEEN 100 AND 109" \
	"CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE events (id bigserial PRIMARY KEY, dim_a_id int, created_at timestamptz NOT NULL, v int NOT NULL, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO events (dim_a_id, created_at, v, pad) SELECT CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 9999 ELSE 1 + (i * 7919) % 650 END,
		timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 60) FROM generate_series(1, 30000) i" \
	"CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, v) WITH (write_buffer_limit = 64, scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)')" \
	"VACUUM (FREEZE, ANALYZE) events, tenant_scope" \
	"CREATE FUNCTION scope_same(k int) RETURNS bool LANGUAGE plpgsql AS \$\$
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
	END \$\$" \
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

# a phase with no edit of the pairs since the build: the reader may read the units under the proof-of-concept switch (the journal makes it unnecessary and removes the switch)
sql scp "SELECT 1" >/dev/null
catchup scp scs
check "a fresh standby reads one key as the join does" test "$(sql scs "SET sift.test_scope_trust = on" "SELECT scope_same(5) AND scope_same(17) AND scope_same(40) AND scope_set_same('{2,9,31}')")" == t

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
ENT="SELECT (SELECT count(*) FROM sift._test_col_scope_entries('events_col')) > 0"
check "the standby holds scope segments" test "$(sql scs "$ENT")" == t

# the scope segments written again, replayed: the standby reads them (stored where the reader may) and exactly
sql scp "REINDEX INDEX events_col" >/dev/null
catchup scp scs
ENTJ="SELECT (SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('events_col'))
	= (SELECT md5(string_agg(e.ctid::text || ':' || s.tenant_id, ',' ORDER BY e.ctid, s.tenant_id)) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id)"
check "after a REINDEX of the primary the scope entries of the standby are the join" test "$(sql scs "$ENTJ")" == t
check "... and its reads are the join's, with the switch and without it" \
	test "$(sql scs "SET sift.test_scope_trust = on" "$SC")" == t -a "$(sql scs "$SC")" == t
check "the index verifies on the standby after the REINDEX" test -z "$(sql scs "SELECT sift.col_verify('events_col', true)" 2>&1)"

node_promote scs
for _ in $(seq 100); do [[ $(sql scs "SELECT pg_is_in_recovery()") == f ]] && break; sleep 0.1; done
sql scs "INSERT INTO events (dim_a_id, created_at, v, pad) SELECT 1 + i % 600, timestamptz '2026-07-01' + i * interval '1 minute', i % 10, 'z' FROM generate_series(1, 700) i" \
	"UPDATE tenant_scope SET tenant_id = 1 + tenant_id % 40 WHERE dim_a_id % 9 = 4 AND tenant_id < 40 AND NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = tenant_scope.dim_a_id AND s.tenant_id = 1 + tenant_scope.tenant_id % 40)" \
	"INSERT INTO tenant_scope VALUES (1, 41), (2, 41), (601, 41) ON CONFLICT DO NOTHING" \
	"SELECT sift.col_promote('events_col')" "SELECT sift.col_merge('events_col', true)" "VACUUM events" >/dev/null
check "the promoted standby takes writes: its index verifies and its scope reads are the join" \
	test -z "$(sql scs "SELECT sift.col_verify('events_col', true)" 2>&1)" -a "$(sql scs "$SC")" == t
finish
