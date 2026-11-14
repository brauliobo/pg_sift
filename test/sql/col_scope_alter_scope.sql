-- ALTER INDEX .. SET (scope = ..) on a sift_col index that has segments would leave them written with another leaf list than the index has, and every scan of it would stop until a REINDEX
-- (the plans of the table go without it). It is refused, and the index goes on serving: the replacement is made beside it (CREATE INDEX CONCURRENTLY, then DROP INDEX). So is RESET (scope).
-- On an index with no segment the ALTER is let through (it has nothing to write again).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE FUNCTION try(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE hint text; BEGIN EXECUTE stmt; RETURN 'ok';
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT; RETURN sqlerrm || coalesce(' [' || hint || ']', ''); END $$;
-- what a statement over the table runs on: the sift index it scans, and the scope it reads
CREATE FUNCTION plan_of(q text, OUT scanned text, OUT scope text) LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO p;
	scanned := jsonb_path_query_first(p, '$.**.Index') #>> '{}';
	scope := jsonb_path_query_first(p, '$.**."Scope Index"') #>> '{}';
END $$;

CREATE TABLE pairs (dim_a_id int NOT NULL, group_id int NOT NULL, PRIMARY KEY (dim_a_id, group_id)) WITH (autovacuum_enabled = false);
INSERT INTO pairs SELECT e, 1 + e % 20 FROM generate_series(1, 400) e;
CREATE INDEX pairs_journal ON pairs USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int) WITH (autovacuum_enabled = false);
-- an index on an empty table has no segment
CREATE INDEX facts_empty ON facts USING sift_col (id, dim_a_id, created_at, amount);
SELECT try('ALTER INDEX facts_empty SET (scope = ''group_id: dim_a_id -> pairs(dim_a_id, group_id)'')') AS empty_table;
SELECT try('ALTER INDEX facts_empty RESET (scope)') AS empty_reset;
DROP INDEX facts_empty;
INSERT INTO facts SELECT i, 1 + i % 450, timestamptz '2026-01-01' + (i % 100) * interval '1 day', i % 10 FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, amount);
VACUUM (FREEZE, ANALYZE) facts;
SET sift.agg_force = on;
SELECT * FROM plan_of('SELECT kind_amount, count(*) FROM (SELECT amount AS kind_amount FROM facts) t GROUP BY 1');

-- the index has segments: the scope is refused, and nothing changed
SELECT try('ALTER INDEX facts_col SET (scope = ''group_id: dim_a_id -> pairs(dim_a_id, group_id)'')') AS with_segments;
SELECT reloptions IS NOT DISTINCT FROM (SELECT reloptions FROM pg_class WHERE relname = 'facts_col') AS unchanged, bool_and(o NOT LIKE 'scope=%') AS no_scope
FROM pg_class c, unnest(coalesce(c.reloptions, '{}')) o WHERE relname = 'facts_col' GROUP BY reloptions;
SELECT * FROM plan_of('SELECT kind_amount, count(*) FROM (SELECT amount AS kind_amount FROM facts) t GROUP BY 1');
-- an option that does not change the leaf list is let through
SELECT try('ALTER INDEX facts_col SET (superunit = 8)') AS other_option;

-- the replacement beside it: no scan is lost while it is built, and it serves once the old one goes
CREATE INDEX CONCURRENTLY facts_scope ON facts USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'group_id: dim_a_id -> pairs(dim_a_id, group_id)');
SELECT * FROM plan_of('SELECT kind_amount, count(*) FROM (SELECT amount AS kind_amount FROM facts) t GROUP BY 1');
DROP INDEX facts_col;
SELECT * FROM plan_of('SELECT kind_amount, count(*) FROM (SELECT amount AS kind_amount FROM facts) t GROUP BY 1');

-- RESET (scope) is refused too: the scope is given up by a replacement without it
SELECT try('ALTER INDEX facts_scope RESET (scope)') AS reset_scope;
SELECT * FROM plan_of('SELECT kind_amount, count(*) FROM (SELECT amount AS kind_amount FROM facts) t GROUP BY 1');

DROP TABLE facts, pairs;
DROP FUNCTION try(text), plan_of(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
