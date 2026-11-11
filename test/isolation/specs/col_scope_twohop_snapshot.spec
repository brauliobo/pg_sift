# A reader of a scope of two paths whose snapshot is older than an edit another session committed after the scope segments were stamped: the changed members are patched under the reader's snapshot, so
# the reader sees the pairs as they were for it (the answer of its own plain join), and a new reader the pairs as they are after the edit. The edit moves a member to another group and gives a
# group a grant; the members of the group are those the memberships list for the reader's snapshot.
# requires: col

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = off);
	CREATE UNIQUE INDEX iso_grants_direct ON iso_grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
	CREATE UNIQUE INDEX iso_grants_group ON iso_grants (group_id, tenant_id) WHERE deleted_at IS NULL;
	CREATE TABLE iso_memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = off);
	CREATE UNIQUE INDEX iso_memberships_active ON iso_memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
	CREATE INDEX iso_grants_by_member ON iso_grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
	CREATE INDEX iso_grants_by_group ON iso_grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
	CREATE INDEX iso_memberships_by_member ON iso_memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
	CREATE TABLE iso_facts (id int PRIMARY KEY, dim_a_id int, amount int) WITH (autovacuum_enabled = off);
	INSERT INTO iso_memberships (group_id, dim_a_id) SELECT 1 + (m - 1) / 10, m FROM generate_series(1, 40) m;
	INSERT INTO iso_grants VALUES (1, 1, NULL, 1, NULL), (2, 2, NULL, 2, NULL), (3, 3, 33, NULL, NULL);
	INSERT INTO iso_facts SELECT i, 1 + i % 45, i % 10 FROM generate_series(1, 2000) i;
	CREATE INDEX iso_facts_col ON iso_facts USING sift_col (id, dim_a_id, amount)
		WITH (scope = 'tenant_id: dim_a_id -> iso_grants(dim_a_id, tenant_id) | iso_grants(group_id, tenant_id) via iso_memberships(group_id, dim_a_id)');
}

setup
{
	REINDEX INDEX iso_facts_col;
}

setup
{
	CREATE FUNCTION iso_read(OUT tenant int, OUT from_units bigint, OUT from_join bigint, OUT units_read boolean) RETURNS SETOF record LANGUAGE plpgsql AS $$
	DECLARE
		p jsonb;
		t int;
	BEGIN
		PERFORM set_config('sift.agg_force', 'on', true);
		FOR t IN 1..3 LOOP
			tenant := t;
			PERFORM set_config('sift.enable_agg', 'on', true);
			EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*) FROM iso_facts f WHERE sift.in_scope(''iso_facts_col''::regclass, ''tenant_id'', f.dim_a_id, %s)', tenant) INTO p;
			units_read := jsonb_path_exists(p, '$.**."Scope Units Read"');
			EXECUTE format('SELECT count(*) FROM iso_facts f WHERE sift.in_scope(''iso_facts_col''::regclass, ''tenant_id'', f.dim_a_id, %s)', tenant) INTO from_units;
			PERFORM set_config('sift.enable_agg', 'off', true);
			EXECUTE format('SELECT count(*) FROM iso_facts f WHERE f.dim_a_id IN (SELECT dim_a_id FROM iso_grants WHERE tenant_id = %1$s AND deleted_at IS NULL AND dim_a_id IS NOT NULL
				UNION SELECT m.dim_a_id FROM iso_grants g JOIN iso_memberships m ON m.group_id = g.group_id WHERE g.tenant_id = %1$s AND g.deleted_at IS NULL AND m.deleted_at IS NULL)', tenant) INTO from_join;
			RETURN NEXT;
		END LOOP;
	END $$;
}

teardown
{
	DROP FUNCTION iso_read();
	DROP TABLE iso_facts, iso_grants, iso_memberships;
}

session reader
step rbegin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) AS snapshot_taken FROM iso_facts WHERE id = 1; }
step rread	{ SELECT * FROM iso_read(); }
step rend	{ COMMIT; }

session writer
step wedit	{ UPDATE iso_memberships SET group_id = 3 WHERE dim_a_id = 5 AND group_id = 1; INSERT INTO iso_grants VALUES (4, 3, NULL, 4, NULL); }
step wundo	{ UPDATE iso_memberships SET group_id = 1 WHERE dim_a_id = 5 AND group_id = 3; DELETE FROM iso_grants WHERE id = 4; }

permutation rread wedit rread
permutation rbegin wedit rread rend rread
permutation rbegin rread wedit rread rend wundo rread
