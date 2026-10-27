-- The cube holds 16 dimensions, and an index of more plain columns than that has them ranked by data once, at CREATE INDEX, when it asks for it (preagg = 'auto'): a column with more than 256
-- values in a unit has no histogram there, so the columns that fit come first (the option preagg is written as 'auto: a, b', which a rebuild keeps), and
-- the answers are the plain plan's. Without the option the columns keep their order.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = notice;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET cpu_tuple_cost = 0.5;
\ir test/agg_preagg_units.sql

-- id, a measure, 14 columns of many values, then 3 of few and a text: 20 columns, 19 of them cube candidates for 16 dimensions; about 4 units
CREATE TABLE cr (id int, v bigint, h1 int, h2 int, h3 int, h4 int, h5 int, h6 int, h7 int, h8 int, h9 int, h10 int, h11 int, h12 int, h13 int, h14 int,
	l1 int, l2 int, l3 int, t text, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE cr ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO cr SELECT i, i % 100, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7, i + 8, i + 9, i + 10, i + 11, i + 12, i + 13, i + 14,
	i % 5, i % 20, i % 3, 'k' || i % 50, repeat('x', 200) FROM generate_series(1, 30000) i;
VACUUM (FREEZE, ANALYZE) cr;

CREATE FUNCTION cr_option(i text) RETURNS text LANGUAGE sql AS
$$ SELECT coalesce((SELECT o FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = i AND o LIKE 'preagg=%'), 'not set') $$;
CREATE FUNCTION cr_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;
CREATE TABLE cr_q (n int, q text);
INSERT INTO cr_q VALUES
	(1, 'SELECT l2, count(*), sum(v) FROM cr GROUP BY 1'),
	(2, 'SELECT count(*), sum(v) FROM cr WHERE l3 = 1'),
	(3, 'SELECT t, count(*) FROM cr GROUP BY 1');
CREATE FUNCTION cr_check() RETURNS TABLE (n int, same bool, from_cube bool) LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
	FOR r IN SELECT * FROM cr_q ORDER BY cr_q.n LOOP
		n := r.n;
		from_cube := (SELECT coalesce(u.pre, 0) > 0 FROM pa_units(r.q) u);
		same := cr_rows(r.q, true) = cr_rows(r.q, false);
		RETURN NEXT;
	END LOOP;
END $$;

-- preagg = 'auto': the columns that fit take the first dimensions
CREATE INDEX cr_auto ON cr USING sift_col (id, v, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11, h12, h13, h14, l1, l2, l3, t) WITH (rollup_dims = 'off', preagg = 'auto');
SELECT cr_option('cr_auto');
SELECT sift.col_verify('cr_auto', true);
SELECT * FROM cr_check();

-- a rebuild keeps the list, whatever the table holds now
UPDATE cr SET h1 = h1 % 7, l1 = l1 + id;
VACUUM (FREEZE, ANALYZE) cr;
REINDEX INDEX cr_auto;
SELECT cr_option('cr_auto');
-- no option is the order of the columns, and auto decides again at the next REINDEX
DROP INDEX cr_auto;
CREATE INDEX cr_order ON cr USING sift_col (id, v, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11, h12, h13, h14, l1, l2, l3, t) WITH (rollup_dims = 'off');
SELECT cr_option('cr_order');
SELECT * FROM cr_check();
ALTER INDEX cr_order SET (preagg = 'auto');
REINDEX INDEX cr_order;
SELECT cr_option('cr_order');
SELECT * FROM cr_check();
SELECT sift.col_verify('cr_order', true);

-- 16 columns or fewer keep their order and write nothing, auto or not
CREATE TABLE cr_few (id int, a int, b int, c text) WITH (autovacuum_enabled = off);
INSERT INTO cr_few SELECT i, i, i % 5, 'k' || i % 7 FROM generate_series(1, 2000) i;
CREATE INDEX cr_few_col ON cr_few USING sift_col (id, a, b, c) WITH (rollup_dims = 'off', preagg = 'auto');
SELECT cr_option('cr_few_col');

DROP TABLE cr, cr_q, cr_few;
DROP FUNCTION cr_option(text), cr_rows(text, bool), cr_check(), pa_units(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
