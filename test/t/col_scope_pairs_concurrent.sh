#!/usr/bin/env bash
# Pairs kept from a view (sift.scope_follow_pairs): two sessions that change different source tables of the view at once. A pair that depends on both changes (a tenant is assigned
# a group in one session, an id joins that group in the other) is computed by neither refresh from the state it needs unless the later refresh waits for the commit of the earlier
# and reads what it wrote: sift.scope_refresh_pairs takes a lock of the pair table for the transaction. Without it the pair is missing until the member or the key is refreshed again.
. "$(dirname "$0")/lib.bash"

plan 5
node_init sp 48
node_start sp || bail "start failed"
sql sp "CREATE EXTENSION pg_sift" \
	"CREATE TABLE group_members (group_id int NOT NULL, dim_a_id int NOT NULL, PRIMARY KEY (group_id, dim_a_id))" \
	"CREATE TABLE links_a (id serial PRIMARY KEY, tenant_id int NOT NULL, group_id int NOT NULL)" \
	"CREATE VIEW visible_dim_a AS SELECT m.dim_a_id, a.tenant_id FROM links_a a JOIN group_members m USING (group_id)" \
	"CREATE TABLE scope_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id))" \
	"SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'links_a', 'key', 'tenant_id')" \
	"SELECT sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'group_members', 'member', 'dim_a_id')" >/dev/null || bail "setup failed"

drift() { sql sp "SELECT count(*) FROM ((SELECT * FROM visible_dim_a EXCEPT SELECT * FROM scope_pairs) UNION ALL (SELECT * FROM scope_pairs EXCEPT SELECT * FROM visible_dim_a)) d"; }
waiting() { sql sp "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND wait_event = 'advisory'"; }

# the holder assigns group 1 to tenant 1 and keeps its transaction open; the other session adds an id to group 1 meanwhile
sql sp "BEGIN" "INSERT INTO links_a (tenant_id, group_id) VALUES (1, 1)" "SELECT pg_sleep(4)" "COMMIT" >/dev/null & HOLDER=$!
sleep 0.5 # timeout-ok: the holder has inserted and sleeps 4 s; the next statement is what is under test
sql sp "INSERT INTO group_members VALUES (1, 10)" >/dev/null & WRITER=$!
check "the second refresh waits for the first one's commit" wait_for '[[ $(waiting) == 1 ]]' 20
wait "$HOLDER" "$WRITER"
check "the pair that depends on both changes is there" test "$(sql sp "SELECT count(*) FROM scope_pairs WHERE dim_a_id = 10 AND tenant_id = 1")" == 1
check "the pairs equal the view" test "$(drift)" == 0

# the other way round: the id leaves while the assignment is moved to another tenant
sql sp "INSERT INTO group_members VALUES (1, 11), (1, 12)" >/dev/null
sql sp "BEGIN" "UPDATE links_a SET tenant_id = 2 WHERE tenant_id = 1" "SELECT pg_sleep(3)" "COMMIT" >/dev/null & HOLDER=$!
sleep 0.5 # timeout-ok: as above
sql sp "DELETE FROM group_members WHERE dim_a_id = 11" >/dev/null & WRITER=$!
wait "$HOLDER" "$WRITER"
check "after the move and the delete the pairs equal the view" test "$(drift)" == 0 -a "$(sql sp "SELECT count(*) FROM scope_pairs")" == 2
check "the move and the delete left tenant 2 with the two ids that stay" test "$(sql sp "SELECT string_agg(dim_a_id::text, ',' ORDER BY dim_a_id) FROM scope_pairs WHERE tenant_id = 2")" == "10,12"
finish
