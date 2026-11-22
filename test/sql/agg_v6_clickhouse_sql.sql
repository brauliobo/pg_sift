-- The analytical SQL of ClickHouse in SiftAggV6 (src/agg/v6/v6_{distinct,argext,filter_pivot,grace_join,asof,datebin_win}.c): sift.arg_max and sift.arg_min, exact COUNT(DISTINCT) of wide and of several columns above work_mem,
-- a FILTER pivot of sixteen branches, date_bin(), and AVG of NUMERIC under a window with FILTER, each held to the SQL of the server that defines it. The data is made by arithmetic, so that the output does not depend on the version.
CREATE EXTENSION pg_sift;
SET timezone = 'UTC';
SET datestyle = 'ISO, MDY';
SET max_parallel_workers = 4;
SET max_parallel_workers_per_gather = 4;
CREATE TABLE ev AS
SELECT i, (i * 7919) % 211 AS g,
       CASE WHEN i % 13 = 0 THEN NULL ELSE timestamptz '2020-01-01 00:00:00+00' + ((i * 104729) % 1000003) * interval '1 second' END AS ts,
       CASE WHEN i % 11 = 0 THEN NULL ELSE 'name' || md5(i::text) END AS name,
       CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 31 % 1000)::numeric / 8 END AS v,
       CASE WHEN i % 17 = 0 THEN NULL ELSE ('\x' || md5(i::text))::bytea END AS b,
       CASE WHEN i % 23 = 0 THEN NULL ELSE 'k' || lpad(((i * 40503) % 1000003)::text, 8, '0') END AS k,
       (i * 40503) % 997 AS c1, (i * 69069) % 89 AS c2, 'u' || (i * 7 % 61) AS c3
FROM generate_series(1::int8, 20000::int8) AS i;
ALTER TABLE ev SET (parallel_workers = 4);
ANALYZE ev;
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- sift.arg_max and sift.arg_min
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- the value of the row with the greatest (least) comparison: the rows without a comparison do not count, a NULL value on the winner is the answer, and a group of no comparison is NULL; k is unique in a group, so the winner is unique
WITH o AS (SELECT DISTINCT ON (g) g, name FROM ev WHERE k IS NOT NULL ORDER BY g, k DESC), a AS (SELECT g, sift.arg_max(name, k) AS name FROM ev GROUP BY g)
SELECT count(*) AS groups, count(*) FILTER (WHERE a.name IS DISTINCT FROM o.name) AS different FROM a JOIN o USING (g);
WITH o AS (SELECT DISTINCT ON (g) g, ts FROM ev WHERE k IS NOT NULL ORDER BY g, k ASC), a AS (SELECT g, sift.arg_min(ts, k) AS ts FROM ev GROUP BY g)
SELECT count(*) AS groups, count(*) FILTER (WHERE a.ts IS DISTINCT FROM o.ts) AS different FROM a JOIN o USING (g);
WITH o AS (SELECT DISTINCT ON (g) g, b FROM ev WHERE ts IS NOT NULL ORDER BY g, ts DESC, i), a AS (SELECT g, sift.arg_max(b, ts) AS b FROM ev GROUP BY g)
SELECT count(*) AS groups, count(*) FILTER (WHERE a.b IS DISTINCT FROM o.b) AS different FROM a JOIN o USING (g);
SELECT sift.arg_max(name, k) IS NOT DISTINCT FROM (SELECT name FROM ev WHERE k IS NOT NULL ORDER BY k DESC LIMIT 1) AS ungrouped_max,
       sift.arg_min(v, k) IS NOT DISTINCT FROM (SELECT v FROM ev WHERE k IS NOT NULL ORDER BY k LIMIT 1) AS ungrouped_min FROM ev;
SELECT sift.arg_max(name, k) AS empty_is_null, sift.arg_min(name, k) AS empty_min_is_null FROM ev WHERE false;
SELECT sift.arg_max(name, k) AS all_null_cmp_is_null FROM ev WHERE k IS NULL;
-- the winner among equal comparisons is the first row of the serial scan
CREATE TABLE tie AS SELECT i, i % 3 AS g, 7 AS c FROM generate_series(1, 30) AS i;
SELECT g, sift.arg_max(i, c) AS first_max, sift.arg_min(i, c) AS first_min FROM tie GROUP BY g ORDER BY g;
-- the aggregate has a combine function: the same answers from partial aggregation in parallel workers
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET debug_parallel_query = on;
WITH o AS (SELECT DISTINCT ON (g) g, name FROM ev WHERE k IS NOT NULL ORDER BY g, k DESC), a AS (SELECT g, sift.arg_max(name, k) AS name FROM ev GROUP BY g)
SELECT count(*) AS groups, count(*) FILTER (WHERE a.name IS DISTINCT FROM o.name) AS different FROM a JOIN o USING (g);
RESET debug_parallel_query;
RESET min_parallel_table_scan_size;
RESET parallel_tuple_cost;
RESET parallel_setup_cost;
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- exact COUNT(DISTINCT) of wide values and of several columns, above work_mem
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
SET work_mem = '64kB';
SELECT count(DISTINCT name) AS names, count(DISTINCT b) AS blobs, count(DISTINCT k) AS keys, count(DISTINCT v) AS numbers FROM ev;
SELECT (SELECT count(*) FROM (SELECT DISTINCT name FROM ev WHERE name IS NOT NULL) AS s) AS names,
       (SELECT count(*) FROM (SELECT DISTINCT b FROM ev WHERE b IS NOT NULL) AS s) AS blobs,
       (SELECT count(*) FROM (SELECT DISTINCT k FROM ev WHERE k IS NOT NULL) AS s) AS keys,
       (SELECT count(*) FROM (SELECT DISTINCT v FROM ev WHERE v IS NOT NULL) AS s) AS numbers;
-- several columns: a NULL in a column is equal to a NULL, as in the comparison of two rows
SELECT count(DISTINCT (c1 % 50, c2 % 20, c3)) AS triples FROM ev;
SELECT count(*) AS triples FROM (SELECT DISTINCT c1 % 50, c2 % 20, c3 FROM ev) AS s;
SELECT g % 5 AS bucket, count(DISTINCT (name, k)) AS pairs FROM ev GROUP BY 1 ORDER BY 1;
SELECT bucket, count(*) AS pairs FROM (SELECT DISTINCT g % 5 AS bucket, name, k FROM ev) AS s GROUP BY 1 ORDER BY 1;
-- numerics that are equal in value are one, whatever their scale
SELECT count(DISTINCT x) AS one_value FROM (VALUES (1.0::numeric), (1.00), (1), (1.000000)) AS t(x);
RESET work_mem;
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- a FILTER pivot of sixteen branches in one pass
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
SELECT count(*) FILTER (WHERE c1 % 16 = 0) AS p0, count(*) FILTER (WHERE c1 % 16 = 1) AS p1, count(*) FILTER (WHERE c1 % 16 = 2) AS p2, count(*) FILTER (WHERE c1 % 16 = 3) AS p3,
       count(*) FILTER (WHERE c1 % 16 = 4) AS p4, count(*) FILTER (WHERE c1 % 16 = 5) AS p5, count(*) FILTER (WHERE c1 % 16 = 6) AS p6, count(*) FILTER (WHERE c1 % 16 = 7) AS p7,
       count(*) FILTER (WHERE c1 % 16 = 8) AS p8, count(*) FILTER (WHERE c1 % 16 = 9) AS p9, count(*) FILTER (WHERE c1 % 16 = 10) AS p10, count(*) FILTER (WHERE c1 % 16 = 11) AS p11,
       count(*) FILTER (WHERE c1 % 16 = 12) AS p12, count(*) FILTER (WHERE c1 % 16 = 13) AS p13, count(*) FILTER (WHERE c1 % 16 = 14) AS p14, count(*) FILTER (WHERE c1 % 16 = 15) AS p15
FROM ev;
SELECT count(*) FILTER (WHERE m = 0) AS p0, count(*) FILTER (WHERE m = 1) AS p1, count(*) FILTER (WHERE m = 2) AS p2, count(*) FILTER (WHERE m = 3) AS p3,
       count(*) FILTER (WHERE m = 4) AS p4, count(*) FILTER (WHERE m = 5) AS p5, count(*) FILTER (WHERE m = 6) AS p6, count(*) FILTER (WHERE m = 7) AS p7,
       count(*) FILTER (WHERE m = 8) AS p8, count(*) FILTER (WHERE m = 9) AS p9, count(*) FILTER (WHERE m = 10) AS p10, count(*) FILTER (WHERE m = 11) AS p11,
       count(*) FILTER (WHERE m = 12) AS p12, count(*) FILTER (WHERE m = 13) AS p13, count(*) FILTER (WHERE m = 14) AS p14, count(*) FILTER (WHERE m = 15) AS p15
FROM (SELECT c1 % 16 AS m FROM ev OFFSET 0) AS s;
-- overlapping branches, a sum, and a distinct count under a branch, grouped
SELECT g % 3 AS grp, count(*) FILTER (WHERE c1 < 500) AS low, count(*) FILTER (WHERE c1 % 2 = 0) AS even, count(*) FILTER (WHERE c1 < 500 AND c1 % 2 = 0) AS both,
       sum(c2) FILTER (WHERE c3 = 'u1') AS sum_u1, count(DISTINCT c1) FILTER (WHERE c2 < 10) AS distinct_low
FROM ev GROUP BY 1 ORDER BY 1;
SELECT grp, sum(CASE WHEN c1 < 500 THEN 1 ELSE 0 END) AS low, sum(CASE WHEN c1 % 2 = 0 THEN 1 ELSE 0 END) AS even, sum(CASE WHEN c1 < 500 AND c1 % 2 = 0 THEN 1 ELSE 0 END) AS both,
       sum(CASE WHEN c3 = 'u1' THEN c2 END) AS sum_u1, (SELECT count(DISTINCT e.c1) FROM ev AS e WHERE e.g % 3 = grp AND e.c2 < 10) AS distinct_low
FROM (SELECT g % 3 AS grp, c1, c2, c3 FROM ev OFFSET 0) AS s GROUP BY grp ORDER BY grp;
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- date_bin()
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
SELECT date_bin(interval '15 minutes', ts, timestamptz '2020-01-01 00:00:00+00') AS bucket, count(*) AS n FROM ev WHERE ts < '2020-01-01 03:00:00+00' GROUP BY 1 ORDER BY 1;
SELECT bucket, count(*) AS n
FROM (SELECT timestamptz '2020-01-01 00:00:00+00' + floor(extract(epoch FROM ts - timestamptz '2020-01-01 00:00:00+00') / 900) * interval '15 minutes' AS bucket FROM ev WHERE ts < '2020-01-01 03:00:00+00') AS s
GROUP BY 1 ORDER BY 1;
SELECT date_bin(interval '1 day', timestamp '2020-01-03 12:00', timestamp '2020-01-01 06:00') AS after_origin, date_bin(interval '1 day', timestamp '2019-12-30 12:00', timestamp '2020-01-01 06:00') AS before_origin;
SELECT date_bin(interval '0 seconds', timestamp '2020-01-03', timestamp '2020-01-01');
SELECT date_bin(interval '1 month', timestamp '2020-01-03', timestamp '2020-01-01');
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- AVG of NUMERIC and a window with FILTER
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
SELECT g % 4 AS grp, avg(v) AS avg_v, avg(v) FILTER (WHERE c1 < 500) AS avg_low, sum(v) / count(v) AS sum_over_count FROM ev GROUP BY 1 ORDER BY 1;
SELECT g, i, v, sum(v) FILTER (WHERE c1 % 2 = 0) OVER w AS running_even, avg(v) OVER w AS running_avg, row_number() OVER w AS rn, rank() OVER (PARTITION BY g ORDER BY c2) AS rk
FROM ev WHERE g < 2 AND i <= 4000 WINDOW w AS (PARTITION BY g ORDER BY i) ORDER BY g, i LIMIT 12;
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- ASOF: the last row of the other table at or before the time of the row
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
CREATE TABLE trades AS SELECT i AS id, i % 5 AS sym, i * 10 + (i % 7) AS t FROM generate_series(1, 200) AS i;
CREATE TABLE quotes AS SELECT i AS id, i % 5 AS sym, i * 13 AS t, i AS px FROM generate_series(1, 150) AS i;
SELECT count(*) AS trades, count(q.px) AS matched, sum(q.px) AS px_sum, sum(q.t) AS t_sum
FROM trades AS tr LEFT JOIN LATERAL (SELECT px, t FROM quotes AS qq WHERE qq.sym = tr.sym AND qq.t <= tr.t ORDER BY qq.t DESC LIMIT 1) AS q ON true;
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- the same statements over an index of Format 2.0: SiftAggV6 runs them and the answer is the heap's
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
CREATE INDEX ev_v6 ON ev USING sift_col (g, i, ts, name, v, k, c1, c2, c3);
RESET client_min_messages;
ANALYZE ev;
CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, E''\n'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION rows_v6(q text) RETURNS text LANGUAGE sql SET sift.enable_agg = on SET sift.agg_force = on AS $$ SELECT rows_md5(q) $$;
CREATE FUNCTION rows_heap(q text) RETURNS text LANGUAGE sql SET sift.enable_agg = off AS $$ SELECT rows_md5(q) $$;
CREATE FUNCTION runs_v6(q text) RETURNS boolean LANGUAGE plpgsql SET sift.enable_agg = on SET sift.agg_force = on AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAggV6)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE TEMP TABLE live_q (n int, q text);
INSERT INTO live_q VALUES
 (1, 'SELECT g, count(DISTINCT c1) AS d1, count(DISTINCT c2) AS d2, count(*) AS n FROM ev GROUP BY g'),
 (2, 'SELECT count(DISTINCT name) AS names, count(DISTINCT k) AS keys, count(DISTINCT c3) AS units FROM ev'),
 (3, 'SELECT g, count(DISTINCT k) AS keys, count(DISTINCT name) AS names FROM ev GROUP BY g'),
 (4, 'SELECT g, count(*) FILTER (WHERE c1 < 500) AS low, count(*) FILTER (WHERE c2 < 40) AS mid, sum(c2) FILTER (WHERE c3 = ''u1'') AS sum_u1 FROM ev GROUP BY g'),
 (5, 'SELECT count(*) FILTER (WHERE c1 < 125) AS p0, count(*) FILTER (WHERE c1 >= 125 AND c1 < 250) AS p1, count(*) FILTER (WHERE c1 >= 250 AND c1 < 500) AS p2, count(*) FILTER (WHERE c1 >= 500) AS p3 FROM ev'),
 (6, 'SELECT date_bin(interval ''15 minutes'', ts, timestamptz ''2020-01-01 00:00:00+00'') AS bucket, count(*) AS n FROM ev WHERE ts < ''2020-01-01 03:00:00+00'' GROUP BY 1'),
 (7, 'SELECT g, sift.arg_max(c1, i) AS m1, sift.arg_min(c2, i) AS m2 FROM ev GROUP BY g'),
 (8, 'SELECT g, avg(c2) AS avg_c2, avg(c2) FILTER (WHERE c1 < 500) AS avg_low FROM ev GROUP BY g');
SELECT n, runs_v6(q) AS runs_in_siftaggv6, rows_v6(q) = rows_heap(q) AS same_rows FROM live_q ORDER BY n;
DROP FUNCTION runs_v6(text);
DROP FUNCTION rows_heap(text);
DROP FUNCTION rows_v6(text);
DROP FUNCTION rows_md5(text);
DROP TABLE quotes;
DROP TABLE trades;
DROP TABLE tie;
DROP TABLE ev;
DROP EXTENSION pg_sift;
