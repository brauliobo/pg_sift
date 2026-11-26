-- The device against the CPU on the two shapes where a list of values meets it: a constant array whose elements are narrower than an int4 or are instants (int2[], int8[] with a NULL,
-- timestamptz[]) read as the set of the scan, and a COUNT(DISTINCT) of thousands of groups whose table does not fit work_mem and spills. The test prints only whether the two agree,
-- so the output is the same on a build without the backend, on one without a device, and on one with a device.
CREATE EXTENSION pg_sift;
SET client_min_messages = error;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET sift.agg_exact_float = on;

CREATE TABLE gi (id int, s int2, w int8, q int, t timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO gi
SELECT i, (i % 100)::int2, i % 100, (i * 31) % 1000, timestamptz '2024-01-01' + (i % 100) * interval '1 day'
-- 120000 rows are 15 granules of 8192 for the constant arrays to be matched against
FROM generate_series(1, 120000) i;
CREATE INDEX gi_col ON gi USING sift_col (id, s, w, q, t);
VACUUM (FREEZE, ANALYZE) gi;
CREATE TABLE gj (LIKE gi) USING sift_table;
INSERT INTO gj SELECT * FROM gi;
VACUUM (FREEZE, ANALYZE) gj;

CREATE TABLE gx (id int, g int, q int) WITH (autovacuum_enabled = false);
-- 250000 rows are 31 granules of 8192 for the device and the CPU to fold each on their own
INSERT INTO gx SELECT i, i % 40, (i * 31) % 1000 FROM generate_series(1, 250000) i;
CREATE INDEX gx_col ON gx USING sift_col (id, g, q);
VACUUM (FREEZE, ANALYZE) gx;
CREATE TABLE gt (LIKE gx) USING sift_table;
INSERT INTO gt SELECT * FROM gx;
VACUUM (FREEZE, ANALYZE) gt;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET sift.gpu = on;
	SET sift.gpu_min_rows = 1;
	EXECUTE format('CREATE TEMP TABLE r_on AS SELECT x::text AS r FROM (%s) x', q);
	SET sift.gpu = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS SELECT x::text AS r FROM (%s) x', q);
	RESET sift.gpu;
	RESET sift.gpu_min_rows;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	d := d + abs((SELECT count(*) FROM r_on) - (SELECT count(*) FROM r_off));
	DROP TABLE r_on, r_off;
	RETURN d = 0;
END $$;

-- the table the query names, and the sift_table copy of it
CREATE FUNCTION same_both(q text) RETURNS bool LANGUAGE sql AS $$
	SELECT same(q) AND same(replace(replace(q, ' gx', ' gt'), ' gi', ' gj'));
$$;

-- the elements of the list are read at their own width and alignment
SELECT same_both($$SELECT s, count(*) FROM gi WHERE s = ANY ('{5,6,7,8}'::int2[]) GROUP BY s$$) AS int2_list;
SELECT same_both($$SELECT s, count(*), sum(q) FROM gi WHERE s = ANY ('{1,3,5,7,9,11,13}'::int2[]) GROUP BY s$$) AS int2_list_seven;
SELECT same_both($$SELECT count(*), sum(q) FROM gi WHERE s = ANY ('{5,6,7,8}'::int2[])$$) AS int2_list_ungrouped;
SELECT same_both($$SELECT w, count(*) FROM gi WHERE w = ANY ('{5,7,8}'::int8[]) GROUP BY w$$) AS int8_list;
SELECT same_both($$SELECT w, count(*) FROM gi WHERE w = ANY ('{5,NULL,7,8}'::int8[]) GROUP BY w$$) AS int8_list_null;
SELECT same_both($$SELECT t, count(*) FROM gi WHERE t = ANY ('{2024-01-03,2024-01-05,2024-02-01}'::timestamptz[]) GROUP BY t$$) AS timestamptz_list;

-- the groups of a COUNT(DISTINCT) that the table could not keep in memory are still groups
SELECT same_both($$SELECT id, count(DISTINCT q) FROM gx WHERE id < 40000 GROUP BY id$$) AS distinct_by_sparse_keys;
SELECT same_both($$SELECT g, count(DISTINCT q) FROM gx GROUP BY g$$) AS distinct_by_dense_keys;

DROP TABLE gi, gj, gx, gt;
DROP FUNCTION same(text), same_both(text);
DROP EXTENSION pg_sift;
