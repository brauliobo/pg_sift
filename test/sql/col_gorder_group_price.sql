-- The build prices the second order of an array by the table of groups each segment writes: 24 bytes for every group the segment holds, or, from 1,024 groups on and with no NULL key in the
-- segment, the bits of a distance from the first group and of the start of its entries (col_gorder_group_packed). An entity of 60,000 values with 5 rows apiece costs the packed table about
-- 2 bytes a row (the second order is chosen, the b-tree of the table names it); the same table with a NULL timestamp in one row of fifty costs the 24-byte table of each segment, about 10
-- bytes a row, and the weighted formula refuses it.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET maintenance_work_mem = '64MB';
\set VERBOSITY terse

CREATE TABLE gp_whole (id int, ent_id int, ts timestamp);
INSERT INTO gp_whole SELECT i, abs(hashint4(i)) % 60000, timestamp '2024-01-01' + (abs(hashint4(i + 7)) % 20000000) * interval '1 second' FROM generate_series(1, 300000) i;
CREATE TABLE gp_nulls (LIKE gp_whole);
INSERT INTO gp_nulls SELECT id, ent_id, CASE WHEN id % 50 = 0 THEN NULL ELSE ts END FROM gp_whole;
CREATE INDEX gp_whole_bt ON gp_whole (ent_id, ts);
CREATE INDEX gp_nulls_bt ON gp_nulls (ent_id, ts);
VACUUM (FREEZE, ANALYZE) gp_whole;
VACUUM (FREEZE, ANALYZE) gp_nulls;
CREATE INDEX gp_whole_ix ON gp_whole USING sift_col (id, ent_id, ts) WITH (key_cut = 'off');
CREATE INDEX gp_nulls_ix ON gp_nulls USING sift_col (id, ent_id, ts) WITH (key_cut = 'off');
SELECT relname, (SELECT option_value FROM pg_options_to_table(reloptions) WHERE option_name = 'order_array_group') AS order_array_group
FROM pg_class WHERE relname IN ('gp_whole_ix', 'gp_nulls_ix') ORDER BY 1;

DROP TABLE gp_whole, gp_nulls;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
