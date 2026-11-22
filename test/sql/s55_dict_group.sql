-- A GROUP BY of text columns of a sift_col index that SiftAgg groups by the dictionary codes of the granules gives the groups of the plan without the index: one key, two keys, NULL keys,
-- keys of a column that is not in the dictionary of every part, count(*) alone, and the answer after rows were added and deleted.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
CREATE TABLE s55_dg (id int, kind text, city text, v int, w float8) WITH (autovacuum_enabled = false);
INSERT INTO s55_dg SELECT g, CASE WHEN g % 11 = 0 THEN NULL ELSE 'kind-' || g % 7 END, CASE WHEN g % 13 = 0 THEN NULL ELSE 'city ' || (g * 31) % 1000 END, g % 101, g / 3.0 FROM generate_series(1, 30000) g;
CREATE INDEX s55_dg_col ON s55_dg USING sift_col (kind, city, v, w);
VACUUM (FREEZE, ANALYZE) s55_dg;
INSERT INTO s55_dg SELECT g, 'kind-' || g % 9, CASE WHEN g % 2 = 0 THEN 'new city ' || g % 50 END, g % 101, g / 3.0 FROM generate_series(30001, 34000) g;
DELETE FROM s55_dg WHERE id % 17 = 0;

CREATE FUNCTION s55_dg_same(q text) RETURNS TABLE (scan text, same bool) LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	got text;
	want text;
BEGIN
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO j;
	scan := CASE WHEN jsonb_path_exists(j, '$.** ? (@."Custom Plan Provider" like_regex "^SiftAgg")') THEN 'sift' ELSE 'normal' END;
	EXECUTE 'SELECT md5(string_agg(t::text, '','' ORDER BY t::text)) FROM (' || q || ') t' INTO got;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(t::text, '','' ORDER BY t::text)) FROM (' || q || ') t' INTO want;
	RESET sift.enable_agg;
	same := got = want;
	RETURN NEXT;
END $$;

SELECT * FROM s55_dg_same('SELECT count(*) FROM s55_dg');
SELECT * FROM s55_dg_same('SELECT kind, count(*), sum(v) FROM s55_dg GROUP BY kind');
SELECT * FROM s55_dg_same('SELECT city, count(*), sum(w) FROM s55_dg GROUP BY city');
SELECT * FROM s55_dg_same('SELECT kind, city, count(*), min(v), max(v) FROM s55_dg GROUP BY kind, city');
DROP TABLE s55_dg;
DROP FUNCTION s55_dg_same(text);
DROP EXTENSION pg_sift;
