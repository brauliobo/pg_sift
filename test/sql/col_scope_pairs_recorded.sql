-- The unique btree that defines the pairs of a scope is recorded when the scope segments are written (the index depends on it) and is the one every read uses: a btree made or rebuilt
-- afterwards does not change which rows are pairs under segments already written. CREATE INDEX and REINDEX choose it (the lowest oid of those that qualify) and say which in a NOTICE;
-- ALTER INDEX leaves it. Two plain unique btrees on the same columns have the same pairs, so what shows is the dependency.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE FUNCTION try(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE hint text; BEGIN EXECUTE stmt; RETURN 'ok';
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT; RETURN sqlerrm || coalesce(' [' || hint || ']', ''); END $$;
CREATE FUNCTION group_read(k int, OUT same boolean, OUT scope_rows bigint, OUT fallback text)
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	join_rows bigint;
BEGIN
	EXECUTE format('EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''group_id'', f.dim_a_id, %s)', k) INTO p;
	fallback := jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}';
	EXECUTE format('SELECT count(*) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''group_id'', f.dim_a_id, %s)', k) INTO scope_rows;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT count(*) FROM facts f WHERE f.dim_a_id IN (SELECT dim_a_id FROM pairs WHERE group_id = %s)', k) INTO join_rows;
	SET LOCAL sift.enable_agg = on;
	same := scope_rows = join_rows;
END $$;
-- the btrees the scope index depends on
CREATE FUNCTION edges() RETURNS text LANGUAGE plpgsql AS
$$ DECLARE r text; BEGIN SELECT string_agg(refobjid::regclass::text, ' ' ORDER BY refobjid::regclass::text) INTO r FROM pg_depend
   WHERE classid = 'pg_class'::regclass AND objid = 'facts_col'::regclass AND refclassid = 'pg_class'::regclass AND deptype = 'n' AND (SELECT relkind FROM pg_class WHERE oid = refobjid) = 'i'; RETURN r; END $$;

CREATE TABLE pairs (dim_a_id int NOT NULL, group_id int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO pairs SELECT e, 1 + e % 20 FROM generate_series(1, 400) e;
CREATE UNIQUE INDEX pairs_a ON pairs (dim_a_id, group_id);
-- a second unique btree on the same columns, made later: a higher oid
CREATE UNIQUE INDEX pairs_b ON pairs (group_id, dim_a_id);
CREATE INDEX pairs_journal ON pairs USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + i % 450, timestamptz '2026-01-01' + (i % 100) * interval '1 day', i % 10 FROM generate_series(1, 20000) i;

-- CREATE INDEX chooses the lowest oid and says so
SET client_min_messages = notice;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'group_id: dim_a_id -> pairs(dim_a_id, group_id)');
SET client_min_messages = warning;
REINDEX INDEX facts_col;
VACUUM (FREEZE, ANALYZE) facts;
SELECT edges();
SELECT * FROM group_read(7);

-- rebuilding the recorded btree gives it a new oid, above the other one: the rule would now choose the other, the record follows the rebuilt btree
REINDEX INDEX CONCURRENTLY pairs_a;
SELECT edges();
SELECT * FROM group_read(7);
-- ALTER INDEX leaves the record alone
ALTER INDEX facts_col SET (scope = 'group_id: dim_a_id -> pairs(dim_a_id, group_id)');
SELECT edges();
SELECT try('DROP INDEX pairs_a') AS drop_recorded;
INSERT INTO pairs VALUES (401, 7), (402, 7);
DELETE FROM pairs WHERE dim_a_id = 14;
SELECT * FROM group_read(7);
-- REINDEX writes the segments again and chooses again by the rule
SET client_min_messages = notice;
REINDEX INDEX facts_col;
SET client_min_messages = warning;
SELECT edges();
SELECT * FROM group_read(7);
SELECT try('DROP INDEX pairs_a') AS drop_no_longer_recorded;

DROP TABLE facts, pairs;
DROP FUNCTION group_read(int), try(text), edges();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
