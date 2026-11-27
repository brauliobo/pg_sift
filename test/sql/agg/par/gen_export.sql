-- A participant of the parallel SiftAggV6 counts the states of its generic aggregates (min of a text) in what it holds: past its share of work_mem it hands its groups to the partitions
-- after a batch, not once at the end of its scan, and the answer is still the heap's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers = 8;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 1;
-- 200 groups of a 4 kB state each hold three times the least share of a participant, over 10 granules: the rows that make the parallel node win
CREATE TABLE facts AS
SELECT i, (i % 200)::int AS category_id, repeat(lpad(to_hex(i), 8, '0'), 512) AS body
FROM generate_series(1, 20000) AS i;
CREATE INDEX facts_col ON facts USING sift_col (category_id, body);
VACUUM (ANALYZE, FREEZE) facts;
SET work_mem = '64kB';
-- the strategy of the parallel node, and more records than each participant handing every group over once
CREATE FUNCTION exported_per_batch(q text, OUT strategy text, OUT exported boolean) LANGUAGE plpgsql AS $f$
DECLARE plan text := ''; line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		plan := plan || line || E'\n';
	END LOOP;
	strategy := substring(plan from 'strategy=([a-z-]+)');
	exported := substring(plan from 'partial_merge_recs=(\d+)')::int > 200 * (substring(plan from 'Workers Launched: (\d+)')::int + 1);
END $f$;
SELECT * FROM exported_per_batch('SELECT category_id, min(body) FROM facts GROUP BY category_id');
CREATE TEMP TABLE par AS SELECT category_id, md5(min(body)) AS m FROM facts GROUP BY category_id;
SET sift.enable_agg = off;
SELECT count(*) AS differ FROM par FULL JOIN (SELECT category_id, md5(min(body)) AS m FROM facts GROUP BY category_id) AS ref USING (category_id) WHERE par.m IS DISTINCT FROM ref.m;
RESET sift.enable_agg;
RESET work_mem;
DROP FUNCTION exported_per_batch(text);
DROP TABLE par, facts;
DROP EXTENSION pg_sift;
