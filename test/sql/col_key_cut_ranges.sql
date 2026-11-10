-- A serial build with key_cut of a table that does not fit one run writes runs that do not overlap in the key (sift_col build spill.c): the rows go to a temporary
-- file for each stretch of the key, cut at quantiles of a sample of the table, and the stretches are written in the order of the key. A key is in one unit,
-- nothing is due after the build, the rows are the heap's for every kind of leaf (NULL keys, text, numeric, float, boolean, date, an array), and a build that
-- the temp_file_limit does not allow, or with the setting off, writes the runs of the table's order.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.vacuum_rewrite_limit = 1;
SET sift.test_col_segment_kb = 1500;

CREATE TABLE kr (id int, tenant_id int, note text, price numeric(10,2), score float8, flag bool, d date, tags text[], pad text) WITH (autovacuum_enabled = false);
INSERT INTO kr SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE (i::bigint * 7919 % 300)::int END, CASE WHEN i % 11 = 0 THEN NULL ELSE 'note ' || (i % 50) END,
	(i % 1000) / 4.0, (i % 777) / 7.0, i % 3 = 0, date '2026-01-01' + i % 200, CASE WHEN i % 5 = 0 THEN NULL ELSE ARRAY['t' || (i % 7), 'u' || (i % 3)] END, md5(i::text) || md5((i + 1)::text)
FROM generate_series(1, 90000) i;
VACUUM (FREEZE, ANALYZE) kr;

CREATE FUNCTION kr_same() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; q text;
BEGIN
	FOREACH q IN ARRAY ARRAY['SELECT count(*), sum(price), min(note), max(d), round(sum(score)::numeric, 3) FROM kr WHERE tenant_id = 42', 'SELECT tenant_id, count(*), sum(price) FROM kr WHERE tenant_id IN (3, 77, 299) OR tenant_id IS NULL GROUP BY 1 ORDER BY 1',
		'SELECT flag, count(*), sum(id) FROM kr WHERE tenant_id BETWEEN 100 AND 130 GROUP BY 1 ORDER BY 1', 'SELECT count(*), sum(id), round(sum(score)::numeric, 3) FROM kr', 'SELECT count(*) FROM kr WHERE tags @> ARRAY[''t3''] AND tenant_id < 50'] LOOP
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL sift.enable_agg = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO a;
		SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO b;
		IF a IS DISTINCT FROM b THEN RETURN false; END IF;
	END LOOP;
	RETURN true;
END $$;
CREATE FUNCTION kr_units(ix regclass) RETURNS numeric LANGUAGE sql AS $$
	SELECT round(point_units::numeric, 1) FROM sift.col_clustering(ix, 1) WHERE path = 'tenant_id' $$;

-- the runs of the table's order: a unit in each run for a key
SET sift.col_build_key_ranges = off;
CREATE INDEX kr_runs ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
SELECT count(*) > 3 AS several_runs FROM sift.col_segments('kr_runs') WHERE kind = 'immutable';
SELECT kr_units('kr_runs') > 3 AS units_in_every_run;
SELECT sift.col_maintenance_due('kr_runs') AS runs_due;
DROP INDEX kr_runs;

-- the runs by ranges: a unit, nothing due
SET sift.col_build_key_ranges = on;
CREATE INDEX kr_idx ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
SELECT count(*) > 3 AS several_runs FROM sift.col_segments('kr_idx') WHERE kind = 'immutable';
SELECT kr_units('kr_idx') AS units_for_a_key;
SELECT sift.col_maintenance_due('kr_idx') AS ranges_due;
SELECT sift.col_verify('kr_idx', true);
SELECT kr_same() AS same_rows;
-- the units of the runs follow each other, as those of a rewrite do
SELECT bool_and(sb_lo = prev_hi + 1) AS numbered_on FROM (SELECT sb_lo, coalesce(lag(sb_hi) OVER (ORDER BY sb_lo), sb_lo - 1) AS prev_hi FROM sift.col_segments('kr_idx') WHERE kind = 'immutable') s;
DROP INDEX kr_idx;

-- a key of floats is cut as well (the order of a float is that of its key: -0 and +0 are the same, NaN last)
UPDATE kr SET score = CASE WHEN id % 89 = 0 THEN 'NaN' WHEN id % 83 = 0 THEN -0.0 WHEN id % 79 = 0 THEN 0.0 ELSE score END;
SET sift.col_build_key_ranges = off;
CREATE INDEX kr_runs ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'score', key_cut_rows = 1200, point_tid_columns = 'id');
SELECT sift.col_maintenance_due('kr_runs') AS float_runs_due;
DROP INDEX kr_runs;
SET sift.col_build_key_ranges = on;
CREATE INDEX kr_idx ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'score', key_cut_rows = 1200);
SELECT count(*) > 3 AS several_runs, sift.col_maintenance_due('kr_idx') AS float_ranges_due FROM sift.col_segments('kr_idx') WHERE kind = 'immutable';
SELECT sift.col_verify('kr_idx', true);
SELECT kr_same() AS same_rows_float_key;
DROP INDEX kr_idx;

-- a temp_file_limit below the rows leaves the runs of the table's order, and the rows are the same
SET temp_file_limit = '1MB';
CREATE INDEX kr_idx ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
SELECT kr_units('kr_idx') > 3 AS runs_of_the_table_order;
SELECT kr_same() AS same_rows_without_ranges;
RESET temp_file_limit;
DROP INDEX kr_idx;

-- so does a disk with less room than the rows need (half of what is free is the most the files take)
SET sift.test_col_temp_free_kb = 1024;
CREATE INDEX kr_idx ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
SELECT kr_units('kr_idx') > 3 AS runs_of_the_table_order;
SELECT kr_same() AS same_rows_without_room;
RESET sift.test_col_temp_free_kb;
DROP INDEX kr_idx;

-- a first run larger than the memory a run of the ranges may take (a large maintenance_work_mem) is cut into runs of that size, still without overlap
SET sift.test_col_segment_kb = 6000;
CREATE INDEX kr_idx ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
CREATE TEMP TABLE kr_first AS SELECT count(*) AS runs FROM sift.col_segments('kr_idx') WHERE kind = 'immutable';
DROP INDEX kr_idx;
SET sift.test_col_spill_run_kb = 1500;
CREATE INDEX kr_idx ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
SELECT count(*) > 2 * (SELECT runs FROM kr_first) AS smaller_runs FROM sift.col_segments('kr_idx') WHERE kind = 'immutable';
DROP TABLE kr_first;
SELECT kr_units('kr_idx') AS units_for_a_key, sift.col_maintenance_due('kr_idx') AS capped_ranges_due;
SELECT sift.col_verify('kr_idx', true);
SELECT kr_same() AS same_rows_capped;
RESET sift.test_col_spill_run_kb;
SET sift.test_col_segment_kb = 1500;
DROP INDEX kr_idx;

-- changes after the build are kept
SET sift.col_build_key_ranges = on;
CREATE INDEX kr_idx ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
INSERT INTO kr SELECT 1000000 + g, g % 300, 'late', 1, 1, true, date '2027-01-01', ARRAY['z'], md5(g::text) FROM generate_series(1, 2000) g;
UPDATE kr SET price = price + 1 WHERE id % 50 = 3;
DELETE FROM kr WHERE id % 97 = 5;
SELECT kr_same() AS same_after_changes;
SET sift.test_col_segment_kb = 1500;
REINDEX INDEX kr_idx;
SELECT kr_units('kr_idx') AS units_after_reindex, kr_same() AS same_after_reindex;
DROP INDEX kr_idx;
CREATE INDEX CONCURRENTLY kr_idx ON kr USING sift_col (id, tenant_id, note, price, score, flag, d, tags, pad) WITH (key_cut = 'tenant_id', key_cut_rows = 1200);
SELECT kr_units('kr_idx') AS units_concurrently, kr_same() AS same_concurrently;

RESET sift.test_col_segment_kb;
DROP TABLE kr;
DROP FUNCTION kr_same(), kr_units(regclass);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
