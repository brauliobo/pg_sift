-- The groups of one key stay in the direct arrays past a million (they were rehashed at 1,048,576) up to the memory limit of the statement, and
-- spill when work_mem is smaller: the result is that of the normal plan at every work_mem, with 1.3 million groups.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE sk_facts (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO sk_facts SELECT g, (hashint4(g) & 2147483647) % 1500000, g % 1000 FROM generate_series(1, 3000000) g;
CREATE INDEX sk_facts_col ON sk_facts USING sift_col (k, v);
VACUUM (FREEZE, ANALYZE) sk_facts;

CREATE FUNCTION groups_summary(wm text) RETURNS TABLE (scan text, groups bigint, rows_in bigint, total bigint, same bool) LANGUAGE plpgsql AS $$
DECLARE
	q text := 'SELECT count(*) AS c, sum(n) AS sn, sum(s) AS ss FROM (SELECT k, count(*) AS n, sum(v) AS s FROM sk_facts GROUP BY k) g';
	j jsonb;
	want record;
	got record;
BEGIN
	EXECUTE format('SET LOCAL work_mem = %L', wm);
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO j;
	scan := CASE WHEN jsonb_path_exists(j, '$.** ? (@."Custom Plan Provider" == "SiftAgg")') THEN 'SiftAgg' ELSE 'normal' END;
	EXECUTE q INTO got;
	SET LOCAL sift.enable_agg = off;
	EXECUTE q INTO want;
	RESET sift.enable_agg;
	groups := got.c;
	rows_in := got.sn;
	total := got.ss;
	same := got = want;
	RETURN NEXT;
END $$;

SELECT * FROM groups_summary('1GB');
SELECT * FROM groups_summary('64MB');
SELECT * FROM groups_summary('4MB');
SELECT * FROM groups_summary('64kB');

DROP FUNCTION groups_summary(text);
DROP TABLE sk_facts;
DROP EXTENSION pg_sift;
