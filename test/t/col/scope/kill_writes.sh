#!/usr/bin/env bash
# The postmaster of a node is killed (SIGKILL, by the pid in its postmaster.pid, with its children) at a random moment of a storm that edits the pairs, the memberships and the facts of two scope indexes
# (one on the direct grants, with soft deleted pairs, and one on the grants of a member and of its groups) while a loop promotes, merges and vacuums, and in every third round while a REINDEX of the scope
# index of two paths runs. After each restart the two indexes verify against the heap, sift.col_scope_verify finds no differing key of either (a key it cannot vouch for is "unverified", not a difference),
# the scope reads of every key are the join, and the maintenance and a REINDEX give back an index that reads its units. col_scope_crash kills at the named publications of the directory; this test kills
# anywhere. SIFT_KILLW_ROUNDS (12, 4 quick) rounds, SIFT_KILLW_MAXSLEEP seconds at most (3) between the start of the storm and the kill.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
. "${SIFT_REPO:?}/test/t/lib/twohop.bash"

ROUNDS=$(knob SIFT_KILLW_ROUNDS 12 4)
MAXSLEEP=$(knob SIFT_KILLW_MAXSLEEP 3 2)
node_init kw 52 "$TWOHOP_CONF
sift.async_maintenance = off
max_parallel_maintenance_workers = 0
shared_buffers = 64MB"
node_start kw || bail "start failed"
D=$(dir kw)
SIFT_TMP=$D leak_expect 200 'terminated by signal 9'
twohop_setup kw || bail "setup failed"
sql kw "CREATE TABLE dpairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false)" \
	"CREATE UNIQUE INDEX dpairs_active ON dpairs (dim_a_id, tenant_id) WHERE deleted_at IS NULL" \
	"INSERT INTO dpairs SELECT m, 1 + (m * 7 + j * 5) % 16 FROM generate_series(1, 200) m, generate_series(0, 2) j WHERE j < m % 4 ON CONFLICT DO NOTHING" \
	"CREATE INDEX dpairs_journal ON dpairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE TABLE one_events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, v int, pad text) WITH (autovacuum_enabled = false)" "INSERT INTO one_events SELECT * FROM hop_events" \
	"CREATE INDEX hop_one ON one_events USING sift_col (id, dim_a_id, created_at, v) WITH (scope = 'tenant_id: dim_a_id -> dpairs(dim_a_id, tenant_id) max_multiplicity(64)')" \
	"REINDEX INDEX hop_one" >/dev/null || bail "one path index failed"
cat >"$D/hd.sql" <<'EOF'
\set m random(1, 200)
\set t random(1, 16)
BEGIN;
INSERT INTO dpairs (dim_a_id, tenant_id) VALUES (:m, :t) ON CONFLICT DO NOTHING;
UPDATE dpairs SET deleted_at = now() WHERE dim_a_id = :m AND tenant_id = :t + 1 AND deleted_at IS NULL;
DELETE FROM dpairs WHERE dim_a_id = :m + 1 AND tenant_id = :t AND deleted_at IS NOT NULL;
COMMIT;
EOF
plan $((ROUNDS * 4 + 1))

ONE_Q="SELECT string_agg(k || '=' || (SELECT count(*) || ':' || coalesce(sum(v), 0) FROM one_events e WHERE sift.in_scope('hop_one'::regclass, 'tenant_id', e.dim_a_id, k)), ',' ORDER BY k) FROM generate_series(1, 17) k"
ONE_P="SELECT string_agg(k || '=' || (SELECT count(*) || ':' || coalesce(sum(v), 0) FROM one_events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM dpairs WHERE tenant_id = k AND deleted_at IS NULL)), ',' ORDER BY k) FROM generate_series(1, 17) k"
ONE_STORED="SELECT count(*), sum(v), min(id), count(DISTINCT dim_a_id) FROM one_events e WHERE sift.in_scope('hop_one'::regclass, 'tenant_id', e.dim_a_id, 5)"
differing() { sql kw "SELECT count(*) FROM sift.col_scope_verify('$1', 1000) WHERE problem <> 'unverified'" | tail -n 1; }
one_same() { [[ $(sql kw "SET sift.agg_force = on" "$ONE_Q" | tail -n 1) == $(sql kw "SET sift.enable_agg = off" "$ONE_P" | tail -n 1) ]]; }
reads_same() { [[ $(sql kw "SELECT bool_and(hop_same(k)) FROM generate_series(1, 17) k" 2>&1 | tail -n 1) == t && $(twohop_reads kw) == same ]] && one_same; }
maintainer() { for _ in $(seq 60); do sql kw "SELECT sift.col_promote('hop_col')" "SELECT sift.col_promote('hop_one')" "SELECT sift.col_merge('hop_col', true)" "VACUUM hop_events, one_events" >/dev/null 2>&1 || break; done; }

cat >"$D/hone.sql" <<'EOF'
\set id random(1, 100000)
\set m random(1, 210)
BEGIN;
INSERT INTO one_events VALUES (:id, :m, now(), :id % 10, 'z') ON CONFLICT (id) DO UPDATE SET dim_a_id = EXCLUDED.dim_a_id, v = one_events.v + 1;
DELETE FROM one_events WHERE id = :id + 7;
COMMIT;
EOF
FILES=($(twohop_files kw) -f "$D/hd.sql@3" -f "$D/hone.sql@2")
for i in $(seq "$ROUNDS"); do
	maintainer & MAINT=$!
	REINDEXER=
	if (( i % 3 == 0 )); then ( for _ in 1 2 3; do sql kw "REINDEX INDEX hop_col" >/dev/null 2>&1 || break; done ) & REINDEXER=$!; fi
	crash_cycle kw "round $i" "$MAXSLEEP" 6 "${FILES[@]}" || break
	wait "$MAINT" 2>/dev/null; [[ -z $REINDEXER ]] || wait "$REINDEXER" 2>/dev/null
	check "round $i: both indexes verify against the heap" test "$(sql kw "SELECT sift.col_verify('hop_col', true)" "SELECT sift.col_verify('hop_one', true)" 2>&1 | grep -c ERROR)" == 0
	check "round $i: sift.col_scope_verify finds no differing key of either index" test "$(differing hop_col)|$(differing hop_one)" == "0|0"
	check "round $i: the scope reads of every key are the join (two paths, one path)" reads_same
	sql kw "REINDEX INDEX hop_col" "REINDEX INDEX hop_one" >/dev/null
	out="$(twohop_why kw)|$(twohop_why kw "$ONE_STORED")|$(differing hop_col)|$(differing hop_one)"
	check "round $i: after a REINDEX the units are read and the scope verifies" test "$out" == "stored|stored|0|0"
done
check "the log has no failed assertion" test "$(grep -c 'TRAP' "$D/postgres.log")" == 0
finish
