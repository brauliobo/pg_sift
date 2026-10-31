-- A scan that outgrows hash memory and gives its groups in the order of the ORDER BY: the partitions of its spill are ranges of the first key, merged in the
-- order of the result and each sorted by itself, so no Sort follows the scan (a spilled scan gave its groups by partition, and the plan sorted them). The first key is
-- an integer, a date or a timestamp, ascending or descending, NULLs first or last, with a value that many groups share (a range of one value cannot be cut) and
-- NULL keys. The partitions are equal widths of the range the zones of the leaf give for the key, or cuts of a stream by a sample when they give none. work_mem
-- 64kB with hash_mem_multiplier 1 puts the limit at the smallest the settings allow.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '60s';
SET client_min_messages = warning;

CREATE TABLE ev (id int, ts timestamp, d date, k int, v int);
-- 60,000 groups of (ts, k): 20,000 minutes, 3 values of k, 1,000 rows without a timestamp, and one minute shared by 10,000 groups
INSERT INTO ev
SELECT i, CASE WHEN i % 60 = 0 THEN NULL WHEN i > 50000 THEN timestamp '2025-03-01' ELSE timestamp '2025-01-01' + (i / 3 % 20000) * interval '1 minute' END,
       date '2024-01-01' + i % 400, CASE WHEN i > 50000 THEN i ELSE i % 3 END, i % 1000
FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, ts, d, k, v);
VACUUM (FREEZE, ANALYZE) ev;

-- the query with the memory given: is there a Sort above the scan, did the scan spill, are the rows the normal plan's, in the same order
CREATE FUNCTION order_case(cols text, keys text, ord text, filt text DEFAULT '') RETURNS TABLE (shape text, sorted boolean, spilled boolean, ordered_by_scan boolean, same boolean) LANGUAGE plpgsql AS $$
DECLARE q text; l text; srt boolean := false; sp boolean := false; ok boolean := false; a text; b text;
BEGIN
	q := format('SELECT %s, count(*) AS c, sum(v) AS s FROM ev %s GROUP BY %s ORDER BY %s', cols, filt, keys, ord);
	PERFORM set_config('work_mem', '64kB', false), set_config('hash_mem_multiplier', '1', false), set_config('enable_hashagg', 'off', false), set_config('enable_sort', 'off', false);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		srt := srt OR l ~ '^\s*(->\s+)?Sort';
		sp := sp OR l ~ '[1-9][0-9]* group table spills';
		ok := ok OR l ~ '\([0-9]+ order keys';
	END LOOP;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'')) FROM (%s) r', q) INTO a;
	RESET work_mem; RESET hash_mem_multiplier; RESET enable_hashagg; RESET enable_sort;
	SET sift.enable_agg = off;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'')) FROM (%s) r', q) INTO b;
	RESET sift.enable_agg;
	RETURN QUERY SELECT ord, srt, sp, ok, a = b;
END $$;

SELECT * FROM order_case('ts, k', 'ts, k', 'ts, k');
SELECT * FROM order_case('ts, k', 'ts, k', 'ts DESC, k');
SELECT * FROM order_case('ts, k', 'ts, k', 'ts DESC NULLS LAST, k DESC');
SELECT * FROM order_case('ts, k', 'ts, k', 'ts NULLS FIRST, k');
SELECT * FROM order_case('d, k, ts', 'd, k, ts', 'd DESC, k, ts');
SELECT * FROM order_case('k, ts', 'k, ts', 'k DESC, ts');
SELECT * FROM order_case('date_trunc(''hour'', ts) AS h, k, ts', 'date_trunc(''hour'', ts), k, ts', 'h DESC, k, ts');
-- a first key that is not a step of a leaf of its type (a timestamp cast to a date): the zones do not give its range, and the stream is cut by a sample
SELECT * FROM order_case('ts::date AS dd, k, ts', 'ts::date, k, ts', 'dd DESC, k, ts');

-- a filter that leaves a tenth of the range the zones give: the ranges that hold the groups do not fit and are cut again by the sample
SELECT * FROM order_case('ts, k', 'ts, k', 'ts DESC, k', 'WHERE ts < timestamp ''2025-01-01 12:00''');

DROP FUNCTION order_case(text, text, text, text);
DROP TABLE ev;
DROP EXTENSION pg_sift;
