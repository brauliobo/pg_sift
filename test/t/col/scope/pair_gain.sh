#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A pair written after the summary was built gives a scope key a member the summary never held: the newest rows of the key (a ring read) must include the rows of that member. Who the key held
# at the build is the summary's own (its nodes): a map of the pairs read from the pair relation after the edit already holds the new pair and tells the member was there, so the read
# returned the newest rows of the key without those of the member it gained. Every edit below is read in a session that never read before it and in one that read before it, and each read is
# the answer of the plain plan.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 7
node_init pg 63 "autovacuum = off"
node_start pg || bail "start failed"
sql pg "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz NOT NULL, kind int NOT NULL, amount int NOT NULL, pad text DEFAULT repeat('x', 1500))" \
	"ALTER TABLE facts ALTER COLUMN pad SET STORAGE PLAIN" \
	"CREATE TABLE pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, deleted_at timestamptz, PRIMARY KEY (dim_a_id, tenant_id))" \
	"CREATE UNIQUE INDEX pairs_active ON pairs (dim_a_id, tenant_id) WHERE deleted_at IS NULL" \
	"INSERT INTO facts (id, dim_a_id, created_at, kind, amount) SELECT i, 1 + i % 400, timestamptz '2026-01-01' + (hashint4(1 + i % 400) & 2147483647) % 40 * interval '30 days'
		+ (hashint4(i) & 2147483647) % 20000 * interval '1 minute', i % 7, (i * 13) % 1000 FROM generate_series(1, 20000) i" \
	"INSERT INTO pairs (dim_a_id, tenant_id) SELECT m, 1 + m % 10 FROM generate_series(1, 400) m" \
	"CREATE INDEX pairs_journal ON pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid')" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at) WITH (scope = 'tenant_id: dim_a_id -> pairs(dim_a_id, tenant_id) max_multiplicity(64)', key_order = 'dim_a_id, created_at')" \
	"VACUUM (FREEZE, ANALYZE) facts, pairs" "REINDEX INDEX facts_col" >/dev/null || bail "setup failed"

SCOPE_ON="SET sift.enable_agg = on; SET sift.agg_force = on; SET sift.enable_col_scan = on"
SCOPE_OFF="SET sift.enable_agg = off; SET sift.enable_col_scan = off"
newest() { echo "SELECT string_agg(id::text, ',' ORDER BY created_at DESC, id DESC) FROM (SELECT f.id, f.created_at FROM facts f
	WHERE f.dim_a_id IN (SELECT dim_a_id FROM pairs WHERE tenant_id = $1 AND deleted_at IS NULL) ORDER BY f.created_at DESC, f.id DESC LIMIT 5) x"; }
scope_read() { sql pg "$SCOPE_ON" "$(newest "$1")"; }
plain_read() { sql pg "$SCOPE_OFF" "$(newest "$1")"; }
same() { [[ $(scope_read "$1") == "$(plain_read "$1")" ]]; }
ring_read() { sql pg "$SCOPE_ON" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $(newest "$1")" | grep -q 'Scope Read: ring'; }
# the answer of a session that reads, lets the edit come from another session, and reads again
warm_same() {
	local edit=$1 t=$2 before after
	sess_open pg warm
	sess_say warm "$SCOPE_ON;"
	sess_say warm "$(newest "$t");"
	sess_say warm "\\! psql -X -q -h '$(dir pg)/sock' -p $(port pg) -U postgres -d postgres -c \"$edit\""
	sess_say warm "$(newest "$t");"
	sess_close warm
	before=$(sed -n 1p "$(dir pg)/warm.out")
	after=$(sed -n 2p "$(dir pg)/warm.out")
	[[ -n $before && $after == "$(plain_read "$t")" ]]
}

check "the newest rows of a key come from its ring before any edit" ring_read 4
sql pg "INSERT INTO pairs VALUES (24, 4)" >/dev/null
check "a member the key gained: a session that never read the key answers as the plain plan" same 4
sql pg "UPDATE pairs SET tenant_id = 5 WHERE dim_a_id = 13 AND tenant_id = 4" >/dev/null
check "a member moved from a key to another: both keys answer as the plain plan" test "$(same 4 && same 5 && echo yes)" == yes
check "a member that came back (a deleted pair written again) answers as the plain plan" test "$(sql pg "UPDATE pairs SET deleted_at = now() WHERE dim_a_id = 24 AND tenant_id = 4" "INSERT INTO pairs VALUES (24, 4) ON CONFLICT (dim_a_id, tenant_id) DO UPDATE SET deleted_at = NULL" >/dev/null; same 4 && echo yes)" == yes
sql pg "SELECT sift.merge('facts_col', true)" >/dev/null
check "a member gained after a merge: a session that read the key before the edit answers as the plain plan" warm_same "INSERT INTO pairs VALUES (35, 4)" 4
check "a member gained: a second key answers as the plain plan" warm_same "INSERT INTO pairs VALUES (46, 8)" 8
check "the index verifies" test -z "$(sql pg "SELECT sift.col_verify('facts_col', true)" 2>&1)"
finish
