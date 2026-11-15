-- `doc ? 'k'` in a FILTER, an OR or a CASE of a document column that declares the paths: the rows that have the key, counted beside the rows, from one chunk of the leaf
-- label: `count(*) FILTER (WHERE attrs ? 'a'), sum(id) FILTER (WHERE attrs ?| array['a', 'b'])` over {rows} documents of about 400 bytes
-- ordered: no
-- set: sift.agg_force = on
--@ setup
DROP TABLE IF EXISTS kf_fact;
CREATE TABLE kf_fact (id int, dim_a_id int, attrs jsonb) WITH (autovacuum_enabled = false);
INSERT INTO kf_fact SELECT g, (g::bigint * 7919 % 1000)::int,
	CASE WHEN g % 400 = 0 THEN '["a", "b"]'::jsonb
	     WHEN g % 3 = 0 THEN jsonb_build_object('a', g % 977, 'pad', repeat('x', 300))
	     WHEN g % 7 = 0 THEN jsonb_build_object('b', g % 977, 'a', null, 'pad', repeat('x', 300))
	     ELSE jsonb_build_object('b', g % 977, 'pad', repeat('x', 300)) END
FROM generate_series(1, {rows}) g;
CREATE INDEX kf_fact_col ON kf_fact USING sift_col (id, dim_a_id, attrs) WITH (paths = 'attrs.a float8, attrs.b float8');
VACUUM (ANALYZE) kf_fact
--@ pg
SELECT count(*) FILTER (WHERE attrs ? 'a'), sum(id) FILTER (WHERE attrs ?| array['a', 'b']) FROM kf_fact
