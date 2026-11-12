-- The scope segments of a scope of two paths hold the DISTINCT union of the direct pairs (grants on a member) and the joined ones (grants on a group, with the members of the group): a member
-- reached directly and through several groups is one pair, so a fact row is in the units of a key once. Every way to know the pairs (relations read once, btrees probed) writes the entries
-- of the SQL UNION; so do promotion and merge. The answers are those of the join, IN and EXISTS in every state of the relations, read from the units or as written
-- (col_scope_twohop_patch reads the units of the changed members).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE FUNCTION try(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE hint text; BEGIN EXECUTE stmt; RETURN 'ok';
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT; RETURN sqlerrm || coalesce(' [' || hint || ']', ''); END $$;

CREATE TABLE grants (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);

-- members 1..58 are in the group 1 + m % 6, every fifth in a second group, every eleventh of them soft deleted from the first; members 59 and 60 are in none
INSERT INTO memberships (group_id, dim_a_id, deleted_at) SELECT 1 + m % 6, m, CASE WHEN m % 11 = 0 THEN now() END FROM generate_series(1, 58) m;
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m + 2) % 6, m FROM generate_series(5, 58, 5) m;
INSERT INTO grants VALUES
	(1, 1, 5, NULL, NULL),		-- tenant 1: member 5 directly, and through its two groups (6 and 2)
	(2, 1, 17, NULL, NULL),
	(3, 1, NULL, 6, NULL),
	(4, 1, NULL, 2, NULL),
	(5, 2, NULL, 3, NULL),		-- tenant 2: a group, and a soft deleted direct grant
	(6, 2, 8, NULL, now()),
	(7, 3, 1, NULL, NULL),		-- tenant 3: three members, and a soft deleted group
	(8, 3, 2, NULL, NULL),
	(9, 3, 3, NULL, NULL),
	(10, 3, NULL, 4, now()),
	(11, 4, NULL, 5, NULL),		-- tenant 4: a group, and a group nobody is in
	(12, 4, NULL, 7, NULL),
	(13, 5, 40, NULL, NULL),	-- tenant 5: a member and a group that has it
	(14, 5, NULL, 5, NULL),
	(15, 6, 50, 3, NULL);		-- tenant 6: one row that is a direct and a joined grant at once
INSERT INTO facts SELECT i, CASE WHEN i % 17 = 0 THEN NULL WHEN i % 19 = 0 THEN 100000 + i ELSE 1 + (i * 7) % 60 END, timestamptz '2026-01-01' + i * interval '9 minutes', i % 10
	FROM generate_series(1, 3000) i;

-- the pairs as SQL says them
CREATE VIEW pair_oracle AS
	SELECT dim_a_id AS member, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL;
SELECT tenant_id, count(*) AS dim_b FROM pair_oracle GROUP BY 1 ORDER BY 1;
SELECT count(*) AS member_5_pairs FROM pair_oracle WHERE member = 5 AND tenant_id = 1;

-- plpgsql, so that nothing refers to the index before it exists
-- the entries of the scope segments against the pairs: the (fact row, key) the join has and they do not, the ones they have and the join does not, and the ones they hold twice
CREATE FUNCTION entries_vs_oracle(OUT missing bigint, OUT extra bigint, OUT repeated bigint, OUT entries bigint) LANGUAGE plpgsql AS
$$ BEGIN
	missing := (SELECT count(*) FROM (SELECT f.ctid, p.tenant_id::int8 FROM facts f JOIN pair_oracle p ON p.member = f.dim_a_id
		EXCEPT SELECT tid, key FROM sift._test_col_scope_entries('facts_col')) t);
	extra := (SELECT count(*) FROM (SELECT tid, key FROM sift._test_col_scope_entries('facts_col')
		EXCEPT SELECT f.ctid, p.tenant_id::int8 FROM facts f JOIN pair_oracle p ON p.member = f.dim_a_id) t);
	entries := (SELECT count(*) FROM sift._test_col_scope_entries('facts_col'));
	repeated := entries - (SELECT count(DISTINCT (tid, key)) FROM sift._test_col_scope_entries('facts_col'));
END $$;

-- the keys 1..7 and the keys 1..6 together, read through the scope marker and as SQL says them (IN, EXISTS, the join of the distinct pairs for the marker that counts per pair): how many differ and how many
-- were read from the entity rows instead of the units
CREATE FUNCTION reads(OUT differ_in int, OUT differ_exists int, OUT differ_pairs int, OUT fell_back int) LANGUAGE plpgsql AS $$
DECLARE
	k int;
	a record;
	b record;
	p jsonb;
BEGIN
	differ_in := 0;
	differ_exists := 0;
	differ_pairs := 0;
	fell_back := 0;
	FOR k IN 1..7 LOOP
		EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*), sum(amount) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, %s)', k) INTO p;
		IF jsonb_path_exists(p, '$.**."Scope Fallback"') THEN fell_back := fell_back + 1; END IF;
		EXECUTE format('SELECT count(*) c, coalesce(sum(amount), 0) s FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, %s)', k) INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE format('SELECT count(*) c, coalesce(sum(amount), 0) s FROM facts f WHERE f.dim_a_id IN (SELECT dim_a_id FROM grants WHERE tenant_id = %1$s AND deleted_at IS NULL AND dim_a_id IS NOT NULL
			UNION ALL SELECT m.dim_a_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.tenant_id = %1$s AND g.deleted_at IS NULL AND m.deleted_at IS NULL)', k) INTO b;
		IF a <> b THEN differ_in := differ_in + 1; END IF;
		EXECUTE format('SELECT count(*) c, coalesce(sum(amount), 0) s FROM facts f WHERE EXISTS (SELECT 1 FROM pair_oracle o WHERE o.member = f.dim_a_id AND o.tenant_id = %s)', k) INTO b;
		IF a <> b THEN differ_exists := differ_exists + 1; END IF;
		SET LOCAL sift.enable_agg = on;
		EXECUTE format('SELECT count(*) c, coalesce(sum(amount), 0) s FROM facts f WHERE sift.in_scope_pairs(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, %s)', k) INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE format('SELECT count(*) c, coalesce(sum(amount), 0) s FROM facts f JOIN pair_oracle o ON o.member = f.dim_a_id WHERE o.tenant_id = %s', k) INTO b;
		IF a <> b THEN differ_pairs := differ_pairs + 1; END IF;
		SET LOCAL sift.enable_agg = on;
	END LOOP;
	-- several keys: once for IN, once for each pair for the join
	EXECUTE 'SELECT count(*) c, coalesce(sum(amount), 0) s FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, ARRAY[1, 2, 3, 4, 5, 6]::int8[])' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT count(*) c, coalesce(sum(amount), 0) s FROM facts f WHERE f.dim_a_id IN (SELECT member FROM pair_oracle WHERE tenant_id <= 6)' INTO b;
	SET LOCAL sift.enable_agg = on;
	IF a <> b THEN differ_in := differ_in + 1; END IF;
END $$;

CREATE FUNCTION why() RETURNS text LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, 1)' INTO p;
	RETURN jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}';
END $$;

-- the build, every way to know the pairs: the three relations read once, their btrees probed, chosen by the price
CREATE INDEX grants_by_member ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
SELECT 'default' AS pairs_known, * FROM entries_vs_oracle();
SET sift.test_conn_fill = 1;
REINDEX INDEX facts_col;
SELECT 'relations read once' AS pairs_known, * FROM entries_vs_oracle();
SET sift.test_conn_fill = 2;
REINDEX INDEX facts_col;
SELECT 'btrees probed' AS pairs_known, * FROM entries_vs_oracle();
SET sift.test_conn_fill = 0;
SET sift.test_col_segment_kb = 64;
REINDEX INDEX facts_col;
SELECT 'many runs' AS pairs_known, * FROM entries_vs_oracle();
RESET sift.test_col_segment_kb;

-- the distinct pairs: the member reached directly and through two groups is one pair, its fact rows are in the units of tenant 1 once
SELECT key AS tenant_id, count(*) AS entries, count(DISTINCT tid) AS fact_rows FROM sift._test_col_scope_entries('facts_col') GROUP BY 1 ORDER BY 1;
SELECT count(*) AS facts_of_member_5, (SELECT count(*) FROM sift._test_col_scope_entries('facts_col') e JOIN facts f ON f.ctid = e.tid WHERE e.key = 1 AND f.dim_a_id = 5) AS entries_of_tenant_1
	FROM facts WHERE dim_a_id = 5;

-- the reads: the answers are the join's, IN and EXISTS; the plan says whether the units were read
SELECT * FROM reads();
SELECT why() AS scope_fallback;
-- a read of several keys that counts a row for each pair needs the units: it is refused with the reason, as for any scope read that cannot use them
SELECT try('SELECT count(*) FROM facts f WHERE sift.in_scope_pairs(''facts_col''::regclass, ''tenant_id'', f.dim_a_id, ARRAY[1, 2]::int8[])') AS per_pair_several_keys;

-- every state of the relations, one after the other and committed: the answers stay the join's
CREATE TABLE states (n int, label text, stmt text);
INSERT INTO states VALUES
	(1, 'soft delete a direct grant', 'UPDATE grants SET deleted_at = now() WHERE id = 1'),
	(2, 'restore it', 'UPDATE grants SET deleted_at = NULL WHERE id = 1'),
	(3, 'soft delete a group grant', 'UPDATE grants SET deleted_at = now() WHERE id = 3'),
	(4, 'a grant on a group of a member that has one directly', 'UPDATE grants SET deleted_at = NULL WHERE id = 3; INSERT INTO grants VALUES (16, 1, NULL, 1, NULL)'),
	(5, 'soft delete a membership', 'UPDATE memberships SET deleted_at = now() WHERE group_id = 6 AND dim_a_id = 5'),
	(6, 'restore it', 'UPDATE memberships SET deleted_at = NULL WHERE group_id = 6 AND dim_a_id = 5'),
	(7, 'move a member to another group', 'UPDATE memberships SET group_id = 4 WHERE group_id = 3 AND dim_a_id = 2'),
	(8, 'a membership of a group nobody has a grant on', 'INSERT INTO memberships (group_id, dim_a_id) VALUES (9, 5)'),
	(9, 'delete a grant', 'DELETE FROM grants WHERE id = 14'),
	(10, 'delete memberships', 'DELETE FROM memberships WHERE group_id = 2'),
	(11, 'a grant to a member by an update of its key', 'UPDATE grants SET tenant_id = 2 WHERE id = 7'),
	(12, 'a group grant moved to another group', 'UPDATE grants SET group_id = 1 WHERE id = 11');
SELECT stmt FROM (SELECT n, 0 AS k, stmt FROM states
	UNION ALL SELECT n, 1, format('SELECT %s AS n, %L AS state, r.* FROM reads() r', n, label) FROM states) t ORDER BY n, k \gexec

-- a rebuild writes the entries of the relations as they are now
REINDEX INDEX facts_col;
SELECT * FROM entries_vs_oracle();
SELECT * FROM reads();

-- rows written after the build are in the write buffer, then in scope segments of a promotion, which expands them by the same union; a merge keeps the entries
INSERT INTO facts SELECT i, 1 + (i * 7) % 60, timestamptz '2026-03-01' + i * interval '9 minutes', i % 10 FROM generate_series(3001, 3400) i;
SELECT sift.col_promote('facts_col') >= 0 AS promoted;
SELECT * FROM entries_vs_oracle();
SELECT * FROM reads();
INSERT INTO facts SELECT i, 1 + (i * 7) % 60, timestamptz '2026-04-01' + i * interval '9 minutes', i % 10 FROM generate_series(3401, 3500) i;
SELECT sift.col_promote('facts_col') >= 0 AS promoted_again;
SELECT sift.col_merge('facts_col', true) >= 0 AS merged;
SELECT * FROM entries_vs_oracle();
SELECT * FROM reads();
SELECT sift.col_verify('facts_col', true);

-- the btrees of the paths may lead with either column: the same pairs when they lead with the member, the key or the via column, read once or probed
CREATE UNIQUE INDEX grants_direct_rev ON grants (tenant_id, dim_a_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group_rev ON grants (tenant_id, group_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX memberships_rev ON memberships (dim_a_id, group_id) WHERE deleted_at IS NULL;
DROP INDEX facts_col;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) using grants_direct_rev | grants(group_id, tenant_id) using grants_group_rev via memberships(group_id, dim_a_id) using memberships_rev');
SELECT 'reversed btrees' AS pairs_known, * FROM entries_vs_oracle();
SET sift.test_conn_fill = 1;
REINDEX INDEX facts_col;
SELECT 'reversed, relations read once' AS pairs_known, * FROM entries_vs_oracle();
SET sift.test_conn_fill = 2;
REINDEX INDEX facts_col;
SELECT 'reversed, btrees probed' AS pairs_known, * FROM entries_vs_oracle();
SET sift.test_conn_fill = 0;
SELECT * FROM reads();
DROP INDEX facts_col;
DROP INDEX grants_direct_rev, grants_group_rev, memberships_rev;

-- a join to the direct relation alone is not the pair set of a scope of two paths: the planner keeps it a join and its rows are the plain ones
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
CREATE FUNCTION direct_join() RETURNS TABLE (scope_read bool, same bool) LANGUAGE plpgsql AS $$
DECLARE q text := 'SELECT count(*), sum(f.amount) FROM facts f JOIN grants g ON g.dim_a_id = f.dim_a_id WHERE g.tenant_id = 1 AND g.deleted_at IS NULL';
	p jsonb; a text; b text;
BEGIN
	SET LOCAL sift.agg_force = on;
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE q INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE q INTO b;
	RETURN QUERY SELECT jsonb_path_exists(p, '$.**."Scope Index"'), a = b;
END $$;
SELECT * FROM direct_join();
DROP FUNCTION direct_join();
DROP INDEX facts_col;

-- the multiplicity limit is the one of the distinct pairs: a member reached by several paths adds no entry (all rows of this table are of member 5, reached by 2 paths here, of one pair)
CREATE TABLE facts_hot AS SELECT i AS id, 5 AS dim_a_id, timestamptz '2026-01-01' + i * interval '9 minutes' AS created_at, 1 AS amount FROM generate_series(1, 3000) i;
SELECT count(*) AS paths_per_row FROM (SELECT dim_a_id AS member, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
	UNION ALL SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL) p WHERE member = 5;
SET sift.scope_max_multiplicity = 1;
SELECT try('CREATE INDEX facts_hot_col ON facts_hot USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = ''tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)'')') AS within_limit;
RESET sift.scope_max_multiplicity;
SELECT count(*) AS entries, count(DISTINCT tid) AS fact_rows FROM sift._test_col_scope_entries('facts_hot_col');

DROP TABLE facts, facts_hot, grants, memberships, states CASCADE;
DROP FUNCTION try(text), entries_vs_oracle(), reads(), why();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
