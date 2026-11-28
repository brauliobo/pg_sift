-- The pull of an unnest decodes the elements of the rows the conditions of the table on its key columns leave, as their chunks store them, and no other: the rows are the normal
-- plan's when those columns hold NULLs, after an UPDATE of them and a DELETE, and in parallel.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
CREATE TABLE fe (id int, d date, n int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO fe SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE date '2020-01-01' + i % 1000 END, i % 50,
	(SELECT jsonb_object_agg(k, 1 + (hashint4(i * 16 + n::int) & 7))
	   FROM unnest(ARRAY['a_1', 'a_2', 'b_1', 'b_2', 'c_1', 'c_2', 'd_1', 'd_2', 'e_1', 'e_2', 'f_1', 'f_2']) WITH ORDINALITY t(k, n))
	FROM generate_series(1, 40000) i;
CREATE INDEX fe_col ON fe USING sift_col (id, d, n, doc);
VACUUM (FREEZE, ANALYZE) fe;
\ir test/common/sql/agg/base.sql
CREATE FUNCTION same(q text) RETURNS TABLE (pushed bool, same bool) LANGUAGE sql AS $$ SELECT pushed(q), runq(q, true) = runq(q, false) $$;
CREATE TABLE fe_q (n serial, q text);
INSERT INTO fe_q (q) VALUES
	($$SELECT e.key, count(*), sum((e.value #>> '{}')::int) FROM fe, jsonb_each(fe.doc) e WHERE d >= date '2021-01-01' AND d < date '2021-02-01' AND e.key <> 'b_1' GROUP BY 1$$),
	($$SELECT e.key, count(*) FROM fe, jsonb_each(fe.doc) e WHERE d >= date '2021-01-01' AND d < date '2021-02-01' GROUP BY 1$$),
	($$SELECT e.key, count(*), sum((e.value #>> '{}')::int) FROM fe, jsonb_each(fe.doc) e WHERE n = 7 GROUP BY 1$$),
	($$SELECT e.key, count(*) FROM fe, jsonb_each(fe.doc) e WHERE d IS NULL GROUP BY 1$$),
	($$SELECT e.key, count(*) FROM fe, jsonb_each(fe.doc) e WHERE n IN (1, 2, 49) AND d < date '2020-06-01' GROUP BY 1$$),
	($$SELECT count(*) FROM fe, jsonb_each(fe.doc) e WHERE n = 1000$$);
SELECT n, s.* FROM fe_q, LATERAL same(q) s ORDER BY n;
-- an update moves some rows into the conditions, a delete ends others
UPDATE fe SET n = 7, d = date '2021-01-15' WHERE id % 1000 = 3;
DELETE FROM fe WHERE id % 1000 = 11;
SELECT n, s.* FROM fe_q, LATERAL same(q) s ORDER BY n;
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0; SET max_parallel_workers_per_gather = 2;
SELECT n, s.* FROM fe_q, LATERAL same(q) s ORDER BY n;
RESET ALL;
DROP TABLE fe, fe_q;
DROP FUNCTION same(text), outcome(text,boolean), runs_in_scan(text), runq(text,boolean), pushed(text);
DROP EXTENSION pg_sift;
