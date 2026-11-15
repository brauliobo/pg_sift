-- The values of a column that is a dictionary of numbers (a few values, thousands of them, NULLs among them) are looked up for the rows a scan reads:
-- the lookup of the 8-byte values by their codes is one gather of four values on AVX2 and the loop of the scalar table elsewhere, and every filter that
-- keeps all the rows, a quarter, an eighth or none of them gives the rows of the normal plan at every SIMD level.
CREATE EXTENSION pg_sift;

SET client_min_messages = warning;
SET sift.agg_preagg = off;

CREATE TABLE dg (id int PRIMARY KEY, f int, g int, narrow numeric(8, 2), wide numeric(10, 2), nul numeric(8, 2)) WITH (autovacuum_enabled = off);
INSERT INTO dg
SELECT i, i % 100, i % 9, (i % 4)::numeric, ((i::bigint * 7919) % 5000)::numeric / 100, CASE WHEN i % 7 = 0 THEN NULL ELSE (i % 13)::numeric END
FROM generate_series(1, 300000) i;
CREATE INDEX dg_col ON dg USING sift_col (id, f, g, narrow, wide, nul);
VACUUM (FREEZE, ANALYZE) dg;

CREATE TABLE dg_q (slug text, sql text);
INSERT INTO dg_q
SELECT slug, 'SELECT g, count(*), sum(narrow), sum(wide), count(nul), min(wide), max(nul), count(DISTINCT wide) FROM dg WHERE ' || cond || ' GROUP BY g'
FROM (VALUES ('every_row_kept', 'f >= 0'), ('a_quarter_kept', 'f < 25'), ('an_eighth_kept', 'f < 8'), ('one_row_in_a_hundred_kept', 'f = 3'),
             ('no_row_kept', 'f < 0')) v (slug, cond);

CREATE FUNCTION dg_answer(q text, scan bool, simd text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('sift.simd', simd, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	EXECUTE 'SELECT coalesce(string_agg(t::text, '';'' ORDER BY t::text), '''') FROM (' || q || ') t' INTO r;
	RETURN r;
END $$;

CREATE FUNCTION dg_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; r bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r OR l ~ 'Custom Scan \(SiftAgg\)';
	END LOOP;
	RETURN r;
END $$;

SELECT slug, dg_scan(sql) AS scan, dg_answer(sql, true, 'avx2') = want AS avx2, dg_answer(sql, true, 'scalar') = want AS scalar
FROM (SELECT slug, sql, dg_answer(sql, false, 'scalar') AS want FROM dg_q) q ORDER BY slug;

DROP FUNCTION dg_answer(text, bool, text), dg_scan(text);
DROP TABLE dg_q, dg;
DROP EXTENSION pg_sift;
