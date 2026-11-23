-- The 6.0 Vulkan path against the CPU: sift.gpu = on (every scan the router may send to the device, no cost model) must give the rows the CPU gives, on
-- a sift_col index and on a sift_table alike, and so must the shapes the device leaves to the CPU. The test prints only whether the two agree, so the output
-- is the same on a build without the backend, on one without a device, and on one with a device. The WARNINGs of sift.gpu = on are not shown.
CREATE EXTENSION pg_sift;
SET client_min_messages = error;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET sift.agg_exact_float = on;

CREATE TABLE gx (id int, g int, c text, w int, q int, a bigint, n int, f float8, wide int, tz int, p numeric(12,2), d date, t timestamp)
	WITH (autovacuum_enabled = false);
INSERT INTO gx
SELECT i, i % 40, (ARRAY['north', 'south', 'east', 'west', 'centre'])[1 + i % 5], (i * 7919) % 100003 - 50000, (i * 31) % 1000,
       (i::bigint * 1000003) % 5000000000 - 2500000000, CASE WHEN i % 1000 = 0 THEN NULL ELSE i % 9 END, (i % 100) / 8.0,
       i % 100, 7, (((i::bigint * 37) % 1000000) - 500000)::numeric / 100,
       DATE '2000-01-01' + (i * 13) % 9000, TIMESTAMP '2000-01-01' + ((i::bigint * 7919) % 700000000) * INTERVAL '1 second'
FROM generate_series(1, 250000) i;
CREATE INDEX gx_col ON gx USING sift_col (id, g, c, w, q, a, n, f, wide, tz, p, d, t);
VACUUM (FREEZE, ANALYZE) gx;

CREATE TABLE gt (LIKE gx) USING sift_table;
INSERT INTO gt SELECT * FROM gx;
VACUUM (FREEZE, ANALYZE) gt;

-- the four settings exist, take their values and refuse a bad one
CREATE FUNCTION rejects(guc text, val text) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('SET %s = %L', guc, val);
	EXECUTE format('RESET %s', guc);
	RETURN false;
EXCEPTION WHEN invalid_parameter_value THEN
	RETURN true;
END $$;
SET sift.gpu = auto;
SET sift.gpu = on;
SET sift.gpu = off;
SET sift.gpu_min_rows = 1;
SET sift.gpu_device = '';
SET sift.gpu_cache_mb = 256;
RESET sift.gpu;
RESET sift.gpu_min_rows;
RESET sift.gpu_device;
RESET sift.gpu_cache_mb;
SELECT rejects('sift.gpu', 'sometimes') AS bad_mode_refused;
SELECT rejects('sift.gpu_min_rows', '-1') AS bad_min_rows_refused;

-- the same rows with the device on and off, in both directions, of the table the name says
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

CREATE FUNCTION same_both(q text) RETURNS bool LANGUAGE sql AS $$
	SELECT same(q) AND same(replace(q, ' gx', ' gt'));
$$;

-- no key
SELECT same_both($$SELECT count(*), sum(w), sum(q), min(q), max(w) FROM gx$$) AS ungrouped;
SELECT same_both($$SELECT count(*), sum(w), sum(q) FROM gx WHERE q >= 100 AND q < 600$$) AS range;
SELECT same_both($$SELECT count(*), sum(a), min(a), max(a) FROM gx WHERE g = 7 AND w > 0$$) AS bigint_extremes;
SELECT same_both($$SELECT count(*), sum(w) FROM gx WHERE g IN (1, 2, 3) OR q = 5$$) AS in_or;
SELECT same_both($$SELECT count(*), sum(w) FILTER (WHERE q > 500), count(n) FROM gx$$) AS filter_clause;
SELECT same_both($$SELECT count(*), sum(w) FROM gx WHERE q > 100000$$) AS none_pass;
SELECT same_both($$SELECT count(*), sum(q), sum(p), min(p), max(p) FROM gx WHERE p > 100.5 AND p <= 4000.25$$) AS numeric_range;
SELECT same_both($$SELECT count(*), min(f), max(f) FROM gx WHERE w < 0$$) AS float_min_max;
SELECT same_both($$SELECT count(*), sum(f), avg(f) FROM gx$$) AS float_sum_exact;
SELECT same_both($$SELECT count(*), sum(w + q), sum(w * 3 - q / 2) FROM gx WHERE q < 900$$) AS expressions;
SELECT same_both($$SELECT count(*), sum(w / NULLIF(q, 0)) FROM gx$$) AS division_guarded;
SELECT same_both($$SELECT count(*), count(n), sum(n) FROM gx WHERE n IS NOT NULL OR g = 3$$) AS null_leaf;
SELECT same_both($$SELECT count(*), sum(EXTRACT(year FROM d)::int), sum(EXTRACT(dow FROM t)::int), min(date_trunc('week', t)) FROM gx$$) AS calendar;

-- the columns whose chunks the device decodes (a chunk a codec wrapped in zstd is the CPU's)
SELECT same_both($$SELECT count(*), sum(w), min(id), max(id) FROM gx WHERE w > 0 AND id < 200000$$) AS device_ungrouped;
SELECT same_both($$SELECT count(*), sum(id), max(w) FROM gx WHERE id IN (5, 77, 1001, 90000, 249999)$$) AS device_in_list;
SELECT same_both($$SELECT count(*), sum(w) FROM gx WHERE id IS NOT NULL AND w <= 0 AND id >= 1000$$) AS device_null_test;
SELECT same_both($$SELECT w, count(*), sum(id) FROM gx WHERE id < 5000 GROUP BY w$$) AS device_hash_key;

-- dense keys
SELECT same_both($$SELECT g, count(*), sum(w), sum(q) FROM gx GROUP BY g$$) AS int_key;
SELECT same_both($$SELECT g, avg(w), min(q), max(q), count(*) FROM gx WHERE q > 10 GROUP BY g$$) AS int_key_avg;
SELECT same_both($$SELECT n, count(*), sum(w) FROM gx GROUP BY n$$) AS null_key;
SELECT same_both($$SELECT c, count(*), sum(w), sum(q) FROM gx WHERE w < 20000 GROUP BY c$$) AS dictionary_key;
SELECT same_both($$SELECT g, n, count(*), sum(q) FROM gx GROUP BY g, n$$) AS two_dense_keys;
SELECT same_both($$SELECT g, count(*), sum(p), avg(p) FROM gx GROUP BY g$$) AS numeric_sum;

-- hashed keys: many groups, several keys, a key that is an expression
SELECT same_both($$SELECT wide, count(*), sum(w) FROM gx GROUP BY wide$$) AS key_100;
SELECT same_both($$SELECT id % 5000, count(*), sum(w), min(q), max(a) FROM gx GROUP BY 1$$) AS key_5000;
SELECT same_both($$SELECT id, count(*), sum(w) FROM gx GROUP BY id$$) AS key_250000;
SELECT same_both($$SELECT g, c, count(*), sum(w) FROM gx GROUP BY g, c$$) AS two_keys;
SELECT same_both($$SELECT g, c, w % 7, q % 3, wide % 4, tz, n, id % 2, count(*), sum(a) FROM gx GROUP BY 1, 2, 3, 4, 5, 6, 7, 8$$) AS eight_keys;
SELECT same_both($$SELECT date_trunc('month', t)::date, count(*), sum(w) FROM gx GROUP BY 1$$) AS month_key;
SELECT same_both($$SELECT d, count(*), sum(p) FROM gx WHERE q < 500 GROUP BY d$$) AS date_key;

-- top N, unsorted input
SELECT same_both($$SELECT id, a FROM gx ORDER BY a DESC LIMIT 25$$) AS top_desc;
SELECT same_both($$SELECT id, a FROM gx WHERE g = 7 ORDER BY a LIMIT 100$$) AS top_asc_filtered;
SELECT same_both($$SELECT id, f, w FROM gx ORDER BY w * 3 + q DESC, id LIMIT 10$$) AS top_expression;
SELECT same_both($$SELECT id, n FROM gx ORDER BY n NULLS FIRST, id LIMIT 10$$) AS top_nulls_first;
SELECT same_both($$SELECT id, a FROM gx ORDER BY a DESC LIMIT 256$$) AS top_256;
SELECT same_both($$SELECT id, a FROM gx ORDER BY a DESC LIMIT 300$$) AS top_over_device_limit;

-- COUNT(DISTINCT) of a column of whole numbers, alone or by dense keys: the pairs are counted on the device
SELECT same_both($$SELECT count(DISTINCT q) FROM gx$$) AS distinct_alone;
SELECT same_both($$SELECT count(DISTINCT n), count(*) FROM gx WHERE q > 300$$) AS distinct_null_values;
SELECT same_both($$SELECT count(DISTINCT w), sum(q) FROM gx WHERE g < 5$$) AS distinct_beside_sum;
SELECT same_both($$SELECT g, count(DISTINCT q) FROM gx GROUP BY g$$) AS distinct_by_group;
SELECT same_both($$SELECT g, n, count(DISTINCT q), sum(w) FROM gx GROUP BY g, n$$) AS distinct_two_keys;
SELECT same_both($$SELECT n, count(DISTINCT w) FROM gx WHERE id > 200000 GROUP BY n$$) AS distinct_null_key;

-- the best K groups of ORDER BY a count or a key LIMIT K (the order of every column given, so that no row is chosen among equals)
SELECT same_both($$SELECT id, count(*) FROM gx GROUP BY id ORDER BY 2 DESC, 1 LIMIT 10$$) AS best_count_then_key;
SELECT same_both($$SELECT id % 5000, count(*), sum(w) FROM gx GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 5$$) AS best_hash;
SELECT same_both($$SELECT wide, id % 7, count(*) FROM gx GROUP BY 1, 2 ORDER BY 3 DESC, 1 DESC, 2 LIMIT 4$$) AS best_two_keys;
SELECT same_both($$SELECT id, count(*), sum(w) FROM gx GROUP BY id ORDER BY 1 DESC LIMIT 7$$) AS best_key_desc;
SELECT same_both($$SELECT n, count(*) FROM gx GROUP BY n ORDER BY 1 NULLS FIRST LIMIT 3$$) AS best_key_nulls_first;
SELECT same_both($$SELECT id, count(*) FROM gx GROUP BY id HAVING count(*) > 0 ORDER BY 2 DESC, 1 LIMIT 6$$) AS best_having;
SELECT same_both($$SELECT id, count(*) FROM gx GROUP BY id ORDER BY 2 DESC, 1 LIMIT 100000$$) AS best_more_than_groups;

-- the granules the zones rule out are not sent, and a date bin is the key of the device
SELECT same_both($$SELECT count(*), sum(w) FROM gx WHERE id > 240000$$) AS zones_prune_ungrouped;
SELECT same_both($$SELECT g, count(*) FROM gx WHERE id < 30000 GROUP BY g$$) AS zones_prune_dense;
SELECT same_both($$SELECT w, count(*) FROM gx WHERE id >= 100000 AND id < 120000 GROUP BY w$$) AS zones_prune_hash;
SELECT same_both($$SELECT count(*) FROM gx WHERE id > 1000000$$) AS zones_prune_all;
SELECT same_both($$SELECT date_bin('7 days', t, TIMESTAMP '2001-01-01'), count(*), sum(w) FROM gx WHERE q < 100 GROUP BY 1$$) AS date_bin_key;
SELECT same_both($$SELECT g, date_bin('1 hour', t, TIMESTAMP '2000-01-01 00:20:00'), count(*) FROM gx WHERE id < 5000 GROUP BY 1, 2$$) AS date_bin_two_keys;

-- shapes the device leaves to the CPU
SELECT same_both($$SELECT g, count(DISTINCT q), count(DISTINCT w) FROM gx GROUP BY g$$) AS distinct_two_columns;
SELECT same_both($$SELECT g, count(DISTINCT c) FROM gx GROUP BY g$$) AS distinct_text;
SELECT same_both($$SELECT id, count(DISTINCT q) FROM gx WHERE id < 40000 GROUP BY id$$) AS distinct_by_sparse_keys;
SELECT same_both($$SELECT g, count(DISTINCT q), sum(w) FROM gx GROUP BY g$$) AS distinct_agg;
SELECT same_both($$SELECT g, string_agg(c, ',' ORDER BY c) FROM gx WHERE id < 200 GROUP BY g$$) AS ordered_agg;
SELECT same_both($$SELECT c, count(*) FROM gx WHERE c LIKE 'n%' GROUP BY c$$) AS text_filter;

-- an error of the query is the CPU's error, on the row the CPU meets first
CREATE FUNCTION error_of(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'none';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLERRM;
END $$;
CREATE FUNCTION same_error(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE on_ text; off_ text;
BEGIN
	SET sift.gpu = on;
	SET sift.gpu_min_rows = 1;
	on_ := error_of(q);
	SET sift.gpu = off;
	off_ := error_of(q);
	RESET sift.gpu;
	RESET sift.gpu_min_rows;
	RETURN on_ = off_ AND off_ <> 'none';
END $$;
SELECT same_error($$SELECT sum(w / q) FROM gx$$) AS division_by_zero;
SELECT same_error($$SELECT sum(a * a * 2) FROM gx$$) AS bigint_overflow;
SELECT same_error($$SELECT sum(q + 2147483000) FROM gx$$) AS int_overflow;

-- units with dead rows or pages that are not all-visible are read by the CPU in the same query
UPDATE gx SET w = w + 1, a = a - 5 WHERE id % 97 = 0;
DELETE FROM gx WHERE id % 1013 = 0;
UPDATE gt SET w = w + 1, a = a - 5 WHERE id % 97 = 0;
DELETE FROM gt WHERE id % 1013 = 0;
SELECT same_both($$SELECT g, count(*), sum(w), sum(q) FROM gx WHERE w < 20000 GROUP BY g$$) AS dirty_grouped;
SELECT same_both($$SELECT count(*), sum(w) FROM gx WHERE q < 500$$) AS dirty_ungrouped;
SELECT same_both($$SELECT id % 5000, count(*), sum(w) FROM gx GROUP BY 1$$) AS dirty_hashed;
SELECT same_both($$SELECT id, a FROM gx ORDER BY a DESC LIMIT 25$$) AS dirty_top;

-- no device (a name no device has, whatever the build and the host): sift.gpu = auto and sift.gpu = on leave every scan to the CPU, with the CPU's rows, no error and the node saying
-- that the device was not used; the router does not even price the device
CREATE FUNCTION headless(q text, mode text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false; used bool := false; d bigint;
BEGIN
	PERFORM set_config('sift.gpu', mode, true), set_config('sift.gpu_device', 'no-such-device', true), set_config('sift.gpu_min_rows', '1', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		scan := scan OR l ~ 'Custom Scan \(Sift';
		used := used OR (l ~ '^ *GPU: ' AND l !~ 'not used');
	END LOOP;
	CREATE TEMP TABLE h_on AS SELECT x::text AS r FROM (SELECT g, count(*), sum(w), max(a) FROM gx GROUP BY g) x;
	PERFORM set_config('sift.gpu', 'off', true);
	CREATE TEMP TABLE h_off AS SELECT x::text AS r FROM (SELECT g, count(*), sum(w), max(a) FROM gx GROUP BY g) x;
	SELECT count(*) INTO d FROM ((TABLE h_on EXCEPT ALL TABLE h_off) UNION ALL (TABLE h_off EXCEPT ALL TABLE h_on)) x;
	DROP TABLE h_on, h_off;
	RETURN format('scan %s, device used %s, rows differing %s', scan, used, d);
END $$;
SELECT mode, headless($q$SELECT g, count(*), sum(w), max(a) FROM gx GROUP BY g$q$, mode) AS cpu_fallback FROM unnest(ARRAY['auto', 'on']) mode ORDER BY 1;
DROP FUNCTION headless(text, text);

DROP FUNCTION same_both(text), same(text), same_error(text), error_of(text), rejects(text, text);
DROP TABLE gt, gx;
DROP EXTENSION pg_sift;
