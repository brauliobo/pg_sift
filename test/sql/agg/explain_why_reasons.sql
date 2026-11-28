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
	('05 json order', $q$SELECT sum((doc->>'f')::float8 * 2) FILTER (WHERE x > 3) FROM er$q$, NULL),
	('06 where',      'SELECT g, count(*) FROM er WHERE random() < 2 GROUP BY g', NULL),
	('07 table key',  'SELECT random() > 2, count(*) FROM et GROUP BY 1', 'random()')
) s(n, q, names), LATERAL sift.explain_why(s.q) w
ORDER BY n;
-- a statement the aggregate does not plan, on a sift_table: the table is the source the index row looks at
SELECT arm, used, slug FROM sift.explain_why('SELECT id FROM et WHERE x * 2 = id');
-- the refused expression of an unnest arm reads a correlated sub-plan, whose value the text of the relation cannot name: the reason names no expression
RESET sift.agg_force;
CREATE TABLE eu (id int, src text, items jsonb);
INSERT INTO eu SELECT i, 's' || i % 5, jsonb_build_array(jsonb_build_object('c', 'c' || i % 3, 'q', i % 4)) FROM generate_series(1, 2000) i;
CREATE INDEX eu_col ON eu USING sift_col (id, src, items);
CREATE TABLE eu_dim (src text);
INSERT INTO eu_dim VALUES ('s1'), ('s2'), ('s2');
VACUUM ANALYZE eu, eu_dim;
CREATE TABLE eu_q (n text, q text);
INSERT INTO eu_q VALUES
	('01 sub-plan key', $q$SELECT i.c, (SELECT count(*) FROM eu_dim d WHERE d.src = e.src) AS d, sum(i.q) FROM eu e CROSS JOIN LATERAL jsonb_to_recordset(e.items) AS i(c text, q int) GROUP BY 1, 2$q$),
	('02 init-plan where', $q$SELECT i.c, sum(i.q) FROM eu e CROSS JOIN LATERAL jsonb_to_recordset(e.items) AS i(c text, q int) WHERE e.id > (SELECT count(*) FROM eu_dim) GROUP BY 1$q$);
SELECT n, w.arm, w.used, w.slug FROM eu_q, LATERAL sift.explain_why(q) w ORDER BY n, w.arm, w.slug;
SELECT n, er_rows(q, true) = er_rows(q, false) AS same_rows FROM eu_q ORDER BY n;
DROP FUNCTION er_rows(text, bool);
DROP TABLE er, et, eu, eu_dim, eu_q;
DROP EXTENSION pg_sift;
