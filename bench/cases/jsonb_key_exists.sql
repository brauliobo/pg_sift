-- doc ? 'k' of a document column that declares the path k: the rows whose k holds a value, and the documents that are an array holding the key or a string equal to it, read again from the heap
-- label: `WHERE attrs ? 'a'` over {rows} documents of about 400 bytes, a third with the key a, 1 in 400 an array that holds it
-- ordered: no
-- set: sift.agg_force = on
--@ setup
DROP TABLE IF EXISTS ke_fact;
CREATE TABLE ke_fact (id int, dim_a_id int, attrs jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ke_fact SELECT g, (g::bigint * 7919 % 1000)::int,
	CASE WHEN g % 400 = 0 THEN '["a", "b"]'::jsonb
	     WHEN g % 3 = 0 THEN jsonb_build_object('a', g % 977, 'pad', repeat('x', 300))
	     ELSE jsonb_build_object('b', g % 977, 'pad', repeat('x', 300)) END
FROM generate_series(1, {rows}) g;
CREATE INDEX ke_fact_col ON ke_fact USING sift_col (id, dim_a_id, attrs) WITH (paths = 'attrs.a float8');
VACUUM (ANALYZE) ke_fact
--@ pg
SELECT count(*) FROM ke_fact WHERE attrs ? 'a' AND dim_a_id < 500
