-- order_array_columns = 'auto' weighs a column of fewer than 1,000 values by the default formula too: ORDER BY d, id LIMIT n OFFSET m over 365 dates takes the rows of the group the
-- offset ends in from the array, where the units read every row; a column of 3 values (groups of a third of the table) or a column that is mostly NULL gains nothing and is left out.
-- The rows are the table's without the index (a copy is the oracle).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE t (id int PRIMARY KEY, d date, d3 date, mostly_null date, k int);
INSERT INTO t SELECT i, DATE '2025-01-01' + (hashint4(i) & 16777215) % 365, DATE '2025-01-01' + (hashint4(i + 7) & 16777215) % 3,
	CASE WHEN i % 1000 = 0 THEN DATE '2025-01-01' + (hashint4(i + 9) & 16777215) % 50 END, (hashint4(i + 11) & 16777215) % 100000 FROM generate_series(1, 120000) i;
CREATE TABLE p AS SELECT * FROM t;
ANALYZE t;
ANALYZE p;
CREATE INDEX t_ix ON t USING sift_col (id, d, d3, mostly_null, k);
SELECT 'd' = ANY (string_to_array(substr(o, 7), ', ')) AS d_chosen, 'd3' = ANY (string_to_array(substr(o, 7), ', ')) AS d3_chosen, 'mostly_null' = ANY (string_to_array(substr(o, 7), ', ')) AS mostly_null_chosen
FROM (SELECT option_value AS o FROM pg_options_to_table((SELECT reloptions FROM pg_class WHERE relname = 't_ix')) WHERE option_name = 'order_array_columns') x;

CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.plan_reads_global_order(q) $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, '','') FROM (%s) r', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(r::text, '','') FROM (%s) r', replace(q, '@', 'p')) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

SET sift.agg_force = on;
SELECT global_order('SELECT id, d FROM t ORDER BY d DESC, id LIMIT 20 OFFSET 10000') AS ties_offset_global_order,
       global_order('SELECT id, d3 FROM t ORDER BY d3 DESC, id LIMIT 20 OFFSET 10000') AS three_values_global_order;
SELECT same('SELECT id, d FROM @ ORDER BY d DESC, id LIMIT 20 OFFSET 10000') AS desc_offset, same('SELECT id, d FROM @ ORDER BY d, id LIMIT 20 OFFSET 30000') AS asc_offset,
       same('SELECT id, d FROM @ ORDER BY d DESC, id LIMIT 20') AS desc_first, same('SELECT id, d FROM @ WHERE k < 50000 ORDER BY d DESC, id LIMIT 20 OFFSET 5000') AS filtered_offset,
       same('SELECT id, d3 FROM @ ORDER BY d3 DESC, id LIMIT 20 OFFSET 10000') AS three_values;
DELETE FROM t WHERE id % 13 = 0;
UPDATE t SET d = d + 1 WHERE id % 29 = 0;
VACUUM t;
DROP TABLE p;
CREATE TABLE p AS SELECT * FROM t;
SELECT same('SELECT id, d FROM @ ORDER BY d DESC, id LIMIT 20 OFFSET 10000') AS desc_offset_after_vacuum, same('SELECT id, d FROM @ ORDER BY d, id LIMIT 20 OFFSET 30000') AS asc_offset_after_vacuum;
SELECT sift.col_verify('t_ix');
DROP TABLE t, p;
DROP FUNCTION global_order(text), same(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
