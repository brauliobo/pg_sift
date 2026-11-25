-- The top-N of the 6.0 node (SiftAggV6TopN) on the Vulkan device: the best rows of the parts the device reads and the tournament of the CPU over the rest (the slab, the parts
-- the device refuses) must give the rows the tournament gives alone, the rows of the aggregate engine and the rows of the heap. The test prints only whether they agree, so the output
-- is the same on a build without the backend, on one without a device, and on one with a device; EXPLAIN says whether the device ran. The WARNINGs of sift.gpu = on are not shown.
CREATE EXTENSION pg_sift;
SET client_min_messages = error;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET sift.test_create_format1 = off;

-- a and w are distinct in every row (7919 and 31 are prime to 250007), so no row ties in the order of either
CREATE TABLE gn (id int, g int, w int, a bigint, n int, d date, t timestamp, f float8) WITH (autovacuum_enabled = false);
INSERT INTO gn
SELECT i, i % 40, ((i * 31) % 250007) - 100000, (i::bigint * 7919) % 250007 * 100000 - 12000000000, CASE WHEN i % 1000 = 0 THEN NULL ELSE i % 9 END,
       DATE '2000-01-01' + (i * 13) % 9000, TIMESTAMP '2000-01-01' + ((i::bigint * 7919) % 700000000) * INTERVAL '1 second', (i % 100) / 8.0
FROM generate_series(1, 250000) i;
CREATE INDEX gn_col ON gn USING sift_col (id, g, w, a, n, d, t, f);
VACUUM (FREEZE, ANALYZE) gn;

-- the rows of a query as the device (the node, the device on), the tournament alone (the plan of the node, run with the device off), the engine of the aggregates (sift.gpu = off) and the heap run it
CREATE FUNCTION setup(how text) RETURNS void LANGUAGE sql AS $$
	SELECT set_config('sift.gpu', CASE WHEN how = 'device' THEN 'on' ELSE 'off' END, true),
	       set_config('sift.gpu_min_rows', '1', true),
	       set_config('sift.enable_agg', CASE WHEN how = 'heap' THEN 'off' ELSE 'on' END, true);
$$;

CREATE FUNCTION rows_of(q text, how text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	IF how = 'tournament' THEN
		EXECUTE format('PREPARE tq AS SELECT coalesce(string_agg(x::text, E''\n''), '''') FROM (%s) x', q);
		PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
		PERFORM setup('device');
		EXECUTE 'EXPLAIN EXECUTE tq';
		PERFORM setup('engine');
		EXECUTE 'EXECUTE tq' INTO r;
		DEALLOCATE tq;
		RETURN r;
	END IF;
	PERFORM setup(how);
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n''), '''') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$
	SELECT rows_of(q, 'device') = rows_of(q, 'tournament') AND rows_of(q, 'tournament') = rows_of(q, 'engine') AND rows_of(q, 'engine') = rows_of(q, 'heap');
$$;

-- the plan is the node, and the node reports what the device did or why it did not run
CREATE FUNCTION explained(q text, with_run bool, token text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE p text;
BEGIN
	PERFORM setup('device');
	FOR p IN EXECUTE format('EXPLAIN (%s COSTS off, TIMING off, SUMMARY off) %s', CASE WHEN with_run THEN 'ANALYZE,' ELSE '' END, q) LOOP
		IF p LIKE '%' || token || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

SELECT explained($$SELECT id, a FROM gn ORDER BY a DESC LIMIT 25$$, false, 'SiftAggV6TopN') AS node_chosen;
SELECT explained($$SELECT id, a FROM gn ORDER BY a DESC LIMIT 25$$, true, 'GPU:') AS gpu_reported;

-- whole numbers, dates and instants, both directions, an offset, a filter
SELECT same($$SELECT id, a FROM gn ORDER BY a DESC LIMIT 25$$) AS desc_25;
SELECT same($$SELECT id, a FROM gn ORDER BY a LIMIT 100$$) AS asc_100;
SELECT same($$SELECT id, w, a FROM gn ORDER BY w DESC LIMIT 1000$$) AS int_desc_1000;
SELECT same($$SELECT id, a FROM gn ORDER BY a LIMIT 25 OFFSET 40$$) AS offset_40;
SELECT same($$SELECT id, a FROM gn WHERE w > 0 ORDER BY a DESC LIMIT 50$$) AS filter_range;
SELECT same($$SELECT id, a FROM gn WHERE g IN (1, 2, 3) ORDER BY a LIMIT 20$$) AS filter_in;
SELECT same($$SELECT id, a FROM gn WHERE n IS NOT NULL AND g = 7 ORDER BY a DESC LIMIT 20$$) AS filter_null_test;
SELECT same($$SELECT id, a FROM gn WHERE w > 100000000 ORDER BY a LIMIT 10$$) AS none_pass;
SELECT same($$SELECT d FROM gn ORDER BY d DESC LIMIT 30$$) AS date_desc;
SELECT same($$SELECT t FROM gn ORDER BY t LIMIT 30$$) AS timestamp_asc;
SELECT same($$SELECT n FROM gn ORDER BY n NULLS FIRST LIMIT 10$$) AS nulls_first;
SELECT same($$SELECT n FROM gn ORDER BY n DESC NULLS LAST LIMIT 10$$) AS nulls_last;
SELECT same($$SELECT id, a FROM gn ORDER BY a DESC LIMIT 300$$) AS over_candidate_block;

-- a parameter in a clause keeps the scan on the CPU; the limit may be one
CREATE FUNCTION same_prepared(stmt text, arg int) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d text; e text;
BEGIN
	EXECUTE 'PREPARE pq (int) AS ' || stmt;
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	PERFORM setup('device');
	EXECUTE format('CREATE TEMP TABLE r_dev AS EXECUTE pq(%s)', arg);
	PERFORM setup('heap');
	EXECUTE format('CREATE TEMP TABLE r_heap AS EXECUTE pq(%s)', arg);
	DEALLOCATE pq;
	SELECT count(*) INTO d FROM ((TABLE r_dev EXCEPT ALL TABLE r_heap) UNION ALL (TABLE r_heap EXCEPT ALL TABLE r_dev)) x;
	DROP TABLE r_dev, r_heap;
	RETURN d::int = 0;
END $$;
SELECT same_prepared($$SELECT id, a FROM gn WHERE g = $1 ORDER BY a DESC LIMIT 25$$, 5) AS parameter_clause;
SELECT same_prepared($$SELECT id, a FROM gn ORDER BY a DESC LIMIT $1$$, 40) AS parameter_limit;

-- rows the parts do not hold yet (the slab) that beat the parts' best, in both directions
INSERT INTO gn
SELECT 300000 + i, i % 40, 200000 + i, 1000000000000 + i, i % 9, DATE '2030-01-01' + i, TIMESTAMP '2040-01-01' + i * INTERVAL '1 second', 0
FROM generate_series(1, 300) i;
INSERT INTO gn
SELECT 400000 + i, i % 40, -200000 - i, -1000000000000 - i, i % 9, DATE '1960-01-01' - i, TIMESTAMP '1960-01-01' - i * INTERVAL '1 second', 0
FROM generate_series(1, 300) i;
SELECT same($$SELECT id, a FROM gn ORDER BY a DESC LIMIT 25$$) AS slab_top;
SELECT same($$SELECT id, a FROM gn ORDER BY a LIMIT 100$$) AS slab_bottom;
SELECT same($$SELECT id, w FROM gn ORDER BY w DESC LIMIT 500$$) AS slab_top_500;
SELECT same($$SELECT d FROM gn ORDER BY d DESC LIMIT 30$$) AS slab_date;
SELECT same($$SELECT t FROM gn WHERE g < 20 ORDER BY t LIMIT 30$$) AS slab_timestamp_filtered;

-- deleted rows (a part with a deleted row is not clean for the heap until a vacuum)
DELETE FROM gn WHERE a > 11500000000 AND id % 3 = 0;
SELECT same($$SELECT id, a FROM gn ORDER BY a DESC LIMIT 50$$) AS deleted_unvacuumed;
VACUUM gn;
SELECT same($$SELECT id, a FROM gn ORDER BY a DESC LIMIT 50$$) AS deleted_vacuumed;
SELECT same($$SELECT id, a FROM gn ORDER BY a LIMIT 50 OFFSET 10$$) AS deleted_vacuumed_asc;

-- an update moves the key of a row
UPDATE gn SET a = 5000000000000 WHERE id = 12345;
UPDATE gn SET a = -5000000000000 WHERE id = 54321;
SELECT same($$SELECT id, a FROM gn ORDER BY a DESC LIMIT 10$$) AS updated_top;
SELECT same($$SELECT id, a FROM gn ORDER BY a LIMIT 10$$) AS updated_bottom;

-- shapes the device leaves to the tournament: a float key, two keys, WITH TIES, a limit past what it keeps
SELECT same($$SELECT id, f FROM gn ORDER BY f DESC, id LIMIT 20$$) AS float_key;
SELECT same($$SELECT id, a FROM gn ORDER BY g, a DESC LIMIT 20$$) AS two_keys;
SELECT same($$SELECT n FROM gn ORDER BY n DESC FETCH FIRST 7 ROWS WITH TIES$$) AS with_ties;
SELECT same($$SELECT id, a FROM gn ORDER BY a DESC LIMIT 100000$$) AS limit_large;

-- the leading key of the index: the order of the parts answers, and the tournament wins
SELECT same($$SELECT id, a FROM gn ORDER BY id DESC LIMIT 25$$) AS index_order_desc;
SELECT same($$SELECT id, a FROM gn ORDER BY id LIMIT 25$$) AS index_order_asc;

DROP FUNCTION same_prepared(text, int), same(text), explained(text, bool, text), rows_of(text, text), setup(text);
DROP TABLE gn;
DROP EXTENSION pg_sift;
