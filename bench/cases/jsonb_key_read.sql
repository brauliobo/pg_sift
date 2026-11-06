-- the value of a key read beside the test of the key, `sum((attrs ->> 'a')::float8) FILTER (WHERE attrs ? 'a')`: a document that is no object reads NULL, the rows that hold a value the path cannot store are read again from the heap
-- label: `sum((attrs ->> 'a')::float8) FILTER (WHERE attrs ? 'a'), count(*) FILTER (WHERE attrs ? 'a')` over {rows} documents of about 400 bytes
-- ordered: no
-- set: sift.agg_force = on
--@ setup
DROP TABLE IF EXISTS kr_fact;
CREATE TABLE kr_fact (id int, dim_a_id int, attrs jsonb) WITH (autovacuum_enabled = false);
INSERT INTO kr_fact SELECT g, (g::bigint * 7919 % 1000)::int,
	CASE WHEN g % 400 = 0 THEN '["a", "b"]'::jsonb
	     WHEN g % 3 = 0 THEN jsonb_build_object('a', g % 977, 'pad', repeat('x', 300))
	     WHEN g % 7 = 0 THEN jsonb_build_object('b', g % 977, 'a', null, 'pad', repeat('x', 300))
	     ELSE jsonb_build_object('b', g % 977, 'pad', repeat('x', 300)) END
FROM generate_series(1, {rows}) g;
CREATE INDEX kr_fact_col ON kr_fact USING sift_col (id, dim_a_id, attrs) WITH (paths = 'attrs.a float8, attrs.b float8');
VACUUM (ANALYZE) kr_fact
--@ pg
SELECT sum((attrs ->> 'a')::float8) FILTER (WHERE attrs ? 'a'), count(*) FILTER (WHERE attrs ? 'a') FROM kr_fact
