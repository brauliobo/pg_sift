-- sift.explain_why names the step of the compile that refused the statement and the expression it refused, instead of the "where" or "no_source" of the
-- index row: a GROUP BY key or an output (key), an aggregate (aggregate), a key read for its JSON type and its value (jsonb), a cast that can raise before a
-- FILTER would drop its row (json_order), a condition (where). A statement on a sift_table has a source. Every refused statement gives the rows of the normal plan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
SET sift.async_maintenance = off;
CREATE TABLE er (id int, g int, x int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO er SELECT i, i % 5, i % 7, jsonb_build_object('f', CASE WHEN i % 50 = 0 THEN to_jsonb('x'::text) ELSE to_jsonb(i % 4 + 0.5) END) FROM generate_series(1, 4000) i;
CREATE INDEX er_col ON er USING sift_col (id, g, x, doc) WITH (paths = 'doc.f float8');
CREATE TABLE et (id int, g int, x int) USING sift_table;
INSERT INTO et SELECT i, i % 5, i % 7 FROM generate_series(1, 4000) i;
VACUUM ANALYZE er, et;
CREATE FUNCTION er_rows(q text, push bool) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r jsonb;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT jsonb_agg(to_jsonb(q) ORDER BY to_jsonb(q)::text) FROM (%s) q', q) INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN to_jsonb('error: ' || SQLERRM);
END $$;
-- the slug of the refused arm, whether its reason names the expression, and the rows (a volatile function in a form whose value is fixed)
SELECT n, w.arm, w.used, w.slug, strpos(w.reason, s.names) > 0 AS names_it, er_rows(s.q, true) = er_rows(s.q, false) AS same_rows
FROM (VALUES
	('01 key',        'SELECT random() > 2, count(*) FROM er GROUP BY 1', 'random()'),
	('02 aggregate',  'SELECT g, sum(x * (random() * 0)::int) FROM er GROUP BY g', 'random()'),
	('03 order by',   'SELECT g, sum(x ORDER BY random()) FROM er GROUP BY g', 'ORDER BY (random())'),
	('04 jsonb',      $q$SELECT sum((doc->>'f')::float8) FILTER (WHERE jsonb_typeof(doc->'f') = 'number'), count(*) FROM er$q$, NULL),
	('05 json order', $q$SELECT sum((doc->>'f')::float8) FILTER (WHERE x > 3) FROM er$q$, NULL),
	('06 where',      'SELECT g, count(*) FROM er WHERE random() < 2 GROUP BY g', NULL),
	('07 table key',  'SELECT random() > 2, count(*) FROM et GROUP BY 1', 'random()')
) s(n, q, names), LATERAL sift.explain_why(s.q) w
ORDER BY n;
-- a statement the aggregate does not plan, on a sift_table: the table is the source the index row looks at
SELECT arm, used, slug FROM sift.explain_why('SELECT id FROM et WHERE x * 2 = id');
DROP FUNCTION er_rows(text, bool);
DROP TABLE er, et;
DROP EXTENSION pg_sift;
