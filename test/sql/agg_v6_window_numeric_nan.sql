-- A numeric NaN is not NULL under SiftAggV6Window: over a sift_col index the window functions over a numeric(p, s) column answer as the heap does -- sum, avg, min, max, the variances, lag, lead and
-- first_value fold or hand back the NaN, count counts it, an ORDER BY puts it above every value (and first in a descending order) with the NaN rows of a partition as peers of each other -- in the three ways the
-- node makes its partitions (the parts sorted by the keys and the order, by the keys only, by neither). A numeric(30, 4), an unconstrained numeric (NaN, +-Infinity, 40 digits) and the functions the node
-- does not take stay with PostgreSQL and answer the same, on a sift_col index and on a sift_table. Every check prints t; plan_ok prints t when the statement ran as SiftAggV6Window.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
	IF current_setting('sift.agg_force', true) IS NOT NULL THEN
		PERFORM set_config('sift.agg_force', 'on', false);
	END IF;
END $$;
SET sift.enable_agg = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE wh (id int PRIMARY KEY, grp int NOT NULL, ts timestamp NOT NULL, n numeric(10,2), m numeric(30,4), x numeric) WITH (autovacuum_enabled = false);
INSERT INTO wh
SELECT i, i % 40, timestamp '2026-01-01' + i * interval '1 minute',
	CASE WHEN i % 40 = 37 THEN 'NaN'::numeric						-- group 37: only NaN
		 WHEN i % 40 = 38 THEN NULL							-- group 38: only NULL
		 WHEN i % 40 = 39 THEN CASE WHEN i % 3 = 0 THEN 'NaN'::numeric END		-- group 39: NaN and NULL
		 WHEN i % 40 = 0 AND i < 400 THEN 'NaN'::numeric				-- group 0: starts with NaN
		 WHEN i % 61 = 0 THEN 'NaN'::numeric
		 WHEN i % 7 = 0 THEN NULL
		 ELSE (i * 37 % 500) / 8.0 END,
	CASE WHEN i % 53 = 1 THEN 'NaN'::numeric WHEN i % 11 = 0 THEN NULL ELSE (i % 977) / 3.0 END,
	CASE i % 59 WHEN 2 THEN 'NaN'::numeric WHEN 3 THEN 'Infinity' WHEN 4 THEN '-Infinity' WHEN 5 THEN NULL
		 WHEN 6 THEN 1234567890123456789012345678901234567890.5 ELSE i / 16.0 END
FROM generate_series(1, 4000) i;
CREATE TABLE wr (LIKE wh INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE wg (LIKE wh INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE wt (LIKE wh INCLUDING ALL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO wr SELECT * FROM wh;
INSERT INTO wg SELECT * FROM wh;
INSERT INTO wt SELECT * FROM wh;
CREATE INDEX wr_sift ON wr USING sift_col (id, grp, ts, n, m, x);
CREATE INDEX wg_sift ON wg USING sift_col (grp, id, ts, n, m, x);
SELECT sift.maintain('wt') IS NOT NULL AS maintained;
VACUUM (FREEZE, ANALYZE) wh, wr, wg, wt;
SELECT count(*) AS rows, count(*) FILTER (WHERE n = 'NaN') AS nan_n, count(*) FILTER (WHERE n IS NULL) AS null_n, count(*) FILTER (WHERE m = 'NaN') AS nan_m,
	count(*) FILTER (WHERE x = 'NaN') AS nan_x, count(*) FILTER (WHERE x = 'Infinity') AS inf_x FROM wh;

CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;

-- the same rows from each sift twin and from the heap (which has no sift index, so it is the truth)
CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS
$$ SELECT same_rows(replace(q, '@T', 'wr'), replace(q, '@T', 'wh')) AND same_rows(replace(q, '@T', 'wg'), replace(q, '@T', 'wh')) AND same_rows(replace(q, '@T', 'wt'), replace(q, '@T', 'wh')) $$;

CREATE FUNCTION plan_ok(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'wr') LOOP n := n + (line LIKE '%SiftAggV6Window%')::int; END LOOP;
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'wg') LOOP n := n + (line LIKE '%SiftAggV6Window%')::int; END LOOP;
	RETURN n = 2;
END $$;

-- the partitions of a sorted window (wg: the parts are sorted by grp and id), of the sort of each partition (wg, ORDER BY n) and of the regroup (wr: the parts are sorted by id)
SELECT parity('SELECT id, sum(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS sum_n;
SELECT parity('SELECT id, avg(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS avg_n;
SELECT parity('SELECT id, min(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS min_n;
SELECT parity('SELECT id, max(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS max_n;
SELECT parity('SELECT id, count(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS count_n;
SELECT parity('SELECT id, lag(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS lag_n;
SELECT parity('SELECT id, lag(n, 2) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS lag2_n;
SELECT parity('SELECT id, lead(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS lead_n;
SELECT parity('SELECT id, lead(n, 3) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS lead3_n;
SELECT parity('SELECT id, first_value(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS first_value_n;
SELECT parity('SELECT id, var_pop(n) OVER (PARTITION BY grp ORDER BY id), stddev_samp(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS variances_n;
SELECT parity('SELECT id, sum(n) OVER w, avg(n) OVER w, min(n) OVER w, max(n) OVER w, count(n) OVER w, lag(n) OVER w, lead(n) OVER w, first_value(n) OVER w FROM @T WINDOW w AS (PARTITION BY grp ORDER BY id)') AS all_in_one_n;
SELECT parity('SELECT id, sum(n) OVER w, min(n) OVER w, max(n) OVER w, count(n) OVER w FROM @T WINDOW w AS (PARTITION BY grp ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)') AS rows_frame_n;
SELECT plan_ok('SELECT id, sum(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS sum_n_plan;
SELECT plan_ok('SELECT id, lag(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS lag_n_plan;
SELECT plan_ok('SELECT id, sum(n) OVER w, min(n) OVER w, max(n) OVER w, lead(n) OVER w, first_value(n) OVER w FROM @T WINDOW w AS (PARTITION BY grp ORDER BY id)') AS all_in_one_n_plan;

-- ORDER BY the numeric: the NaN rows are the greatest of their partition and peers of each other
SELECT parity('SELECT id, row_number() OVER (PARTITION BY grp ORDER BY n, id) FROM @T') AS row_number_n;
SELECT parity('SELECT id, row_number() OVER (PARTITION BY grp ORDER BY n DESC NULLS LAST, id) FROM @T') AS row_number_n_desc;
SELECT parity('SELECT id, row_number() OVER (PARTITION BY grp ORDER BY n NULLS FIRST, id) FROM @T') AS row_number_n_nulls_first;
SELECT parity('SELECT id, row_number() OVER (PARTITION BY grp ORDER BY n DESC, id) FROM @T') AS row_number_n_desc_nulls_first;
SELECT parity('SELECT id, rank() OVER (PARTITION BY grp ORDER BY n), dense_rank() OVER (PARTITION BY grp ORDER BY n) FROM @T') AS rank_n;
SELECT parity('SELECT id, rank() OVER (PARTITION BY grp ORDER BY n DESC), dense_rank() OVER (PARTITION BY grp ORDER BY n DESC) FROM @T') AS rank_n_desc;
SELECT parity('SELECT id, sum(n) OVER (PARTITION BY grp ORDER BY n), count(n) OVER (PARTITION BY grp ORDER BY n) FROM @T') AS peers_sum_n;
SELECT parity('SELECT id, min(n) OVER (PARTITION BY grp ORDER BY n DESC), max(n) OVER (PARTITION BY grp ORDER BY n DESC) FROM @T') AS peers_minmax_n_desc;
SELECT parity('SELECT id, grp, n, rn FROM (SELECT id, grp, n, row_number() OVER (PARTITION BY grp ORDER BY n DESC NULLS LAST, id) rn FROM @T) w WHERE rn <= 3') AS top3_n;
SELECT parity('SELECT id, grp, n, rn FROM (SELECT id, grp, n, rank() OVER (PARTITION BY grp ORDER BY n DESC) rn FROM @T) w WHERE rn <= 2') AS rank_top2_n;
SELECT plan_ok('SELECT id, row_number() OVER (PARTITION BY grp ORDER BY n, id) FROM @T') AS row_number_n_plan;
SELECT plan_ok('SELECT id, rank() OVER (PARTITION BY grp ORDER BY n DESC) FROM @T') AS rank_n_desc_plan;
SELECT plan_ok('SELECT id, sum(n) OVER (PARTITION BY grp ORDER BY n) FROM @T') AS peers_sum_n_plan;
SELECT plan_ok('SELECT id, grp, n, rn FROM (SELECT id, grp, n, row_number() OVER (PARTITION BY grp ORDER BY n DESC NULLS LAST, id) rn FROM @T) w WHERE rn <= 3') AS top3_n_plan;

-- the functions and the columns the node leaves to PostgreSQL
SELECT parity('SELECT id, last_value(n) OVER (PARTITION BY grp ORDER BY id), nth_value(n, 2) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS last_nth_n;
SELECT parity('SELECT id, n, count(*) OVER () FROM @T WHERE grp = 3 ORDER BY id LIMIT 20') AS page_n;
SELECT parity('SELECT id, n, count(*) OVER (), sum(n) OVER (), max(n) OVER () FROM @T WHERE grp = 39 ORDER BY id LIMIT 20') AS page_sum_n;
SELECT parity('SELECT id, sum(n) OVER (ORDER BY id), count(n) OVER (ORDER BY id) FROM @T') AS no_partition_n;
SELECT parity('SELECT id, sum(m) OVER (PARTITION BY grp ORDER BY id), min(m) OVER (PARTITION BY grp ORDER BY id), lag(m) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS m;
SELECT parity('SELECT id, row_number() OVER (PARTITION BY grp ORDER BY m, id) FROM @T') AS row_number_m;
SELECT parity('SELECT id, sum(x) OVER (PARTITION BY grp ORDER BY id), max(x) OVER (PARTITION BY grp ORDER BY id), lag(x) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS x;
SELECT parity('SELECT id, row_number() OVER (PARTITION BY grp ORDER BY x DESC, id), rank() OVER (PARTITION BY grp ORDER BY x) FROM @T') AS row_number_x;
SELECT parity('SELECT id, count(x) OVER w, min(x) OVER w, first_value(x) OVER w FROM @T WINDOW w AS (PARTITION BY grp ORDER BY id)') AS count_min_x;

-- the rows inserted after the index was made, not merged into its parts yet, a delete and an update: the NaN rows of the index follow the heap
INSERT INTO wh SELECT i, i % 40, timestamp '2026-02-01' + i * interval '1 minute', CASE i % 5 WHEN 0 THEN 'NaN'::numeric WHEN 1 THEN NULL ELSE i / 4.0 END, 'NaN'::numeric, 'Infinity'
FROM generate_series(5001, 5300) i;
INSERT INTO wr SELECT * FROM wh WHERE id > 5000;
INSERT INTO wg SELECT * FROM wh WHERE id > 5000;
INSERT INTO wt SELECT * FROM wh WHERE id > 5000;
SELECT parity('SELECT id, sum(n) OVER (PARTITION BY grp ORDER BY id), min(n) OVER (PARTITION BY grp ORDER BY id), lag(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS after_insert;
SELECT parity('SELECT id, rank() OVER (PARTITION BY grp ORDER BY n DESC), row_number() OVER (PARTITION BY grp ORDER BY n, id) FROM @T') AS after_insert_order;
DELETE FROM wr WHERE id % 97 = 0;
DELETE FROM wg WHERE id % 97 = 0;
DELETE FROM wt WHERE id % 97 = 0;
DELETE FROM wh WHERE id % 97 = 0;
UPDATE wr SET n = 'NaN' WHERE id % 89 = 0;
UPDATE wg SET n = 'NaN' WHERE id % 89 = 0;
UPDATE wt SET n = 'NaN' WHERE id % 89 = 0;
UPDATE wh SET n = 'NaN' WHERE id % 89 = 0;
SELECT parity('SELECT id, sum(n) OVER (PARTITION BY grp ORDER BY id), max(n) OVER (PARTITION BY grp ORDER BY id), lag(n) OVER (PARTITION BY grp ORDER BY id) FROM @T') AS after_dml;
SELECT parity('SELECT id, row_number() OVER (PARTITION BY grp ORDER BY n DESC, id), rank() OVER (PARTITION BY grp ORDER BY n) FROM @T') AS after_dml_order;
DROP TABLE wh, wr, wg, wt;
DROP FUNCTION plan_ok(text), parity(text), same_rows(text, text);
DROP EXTENSION pg_sift;
