-- A joined table of text, varchar, date, timestamp, timestamptz, int2, bigint, bool and numeric columns (a NaN included) is a lookup join of SiftAggV6, whether
-- its sift_col index covers the columns or not: the rows are those of the snapshot (a row changed by the transaction, a dead row and a row of the write buffer
-- included) and the answers are those of the normal plan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.backend_cache_kb = 0;
SET sift.shared_cache_kb = 0;
SET sift.shared_cache_kb = 0;

CREATE TABLE ev (id int, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 3200, i % 11 FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v);

CREATE TABLE dm (k int PRIMARY KEY, t text, vc varchar(20), d date, ts timestamp, tz timestamptz, i2 int2, i8 bigint, b bool, n numeric(12,2), grp int) WITH (autovacuum_enabled = false);
INSERT INTO dm SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE 'kind' || i % 9 END, CASE WHEN i % 13 = 0 THEN NULL ELSE 'v' || i % 5 END,
	date '2026-01-01' + i % 200, timestamp '2026-01-01 00:00' + (i % 500) * interval '1 hour', timestamptz '2026-01-01 00:00+00' + (i % 400) * interval '1 hour',
	(i % 20 - 5)::int2, i::bigint * 100000, i % 3 = 0, CASE WHEN i % 101 = 0 THEN 'NaN'::numeric WHEN i % 19 = 0 THEN NULL ELSE (i % 1000) / 4.0 END, i % 7
FROM generate_series(0, 3199) i;
CREATE INDEX dm_col ON dm USING sift_col (k, t, vc, d, ts, tz, i2, i8, b, n, grp);
-- an index that leaves the grouping column out
CREATE TABLE dn (k int PRIMARY KEY, t text, grp int) WITH (autovacuum_enabled = false);
INSERT INTO dn SELECT i, 'kind' || i % 9, i % 7 FROM generate_series(0, 3199) i;
CREATE INDEX dn_col ON dn USING sift_col (k, t);
-- the same rows without a NaN
CREATE TABLE dp (LIKE dm INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO dp SELECT * FROM dm WHERE n IS NULL OR n <> 'NaN';
CREATE INDEX dp_col ON dp USING sift_col (k, t, vc, d, ts, tz, i2, i8, b, n, grp);
VACUUM (FREEZE, ANALYZE) ev, dm, dn, dp;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dm.grp, count(*), sum(ev.v) FROM ev JOIN dm ON dm.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT dm.t, count(*), sum(ev.v) FROM ev JOIN dm ON dm.k = ev.k WHERE dm.t = 'kind3' GROUP BY 1 ORDER BY 1$q$),
 (3, $q$SELECT dm.vc, dm.d, count(*) FROM ev JOIN dm ON dm.k = ev.k AND dm.d < date '2026-02-10' AND dm.i2 >= 3 GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (4, $q$SELECT dm.grp, min(dm.ts), max(dm.tz), max(dm.i8), sum(dm.n), count(dm.b), min(dm.t), max(dm.vc) FROM ev JOIN dm ON dm.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (5, $q$SELECT dm.grp, count(*) FROM ev JOIN dm ON dm.k = ev.k AND dm.t <> 'kind1' AND dm.ts >= timestamp '2026-01-10' AND dm.tz < timestamptz '2026-01-20 00:00+00' GROUP BY 1 ORDER BY 1$q$),
 (6, $q$SELECT dm.n, count(*) FROM ev JOIN dm ON dm.k = ev.k AND dm.i8 > 100000000 GROUP BY 1 ORDER BY 1 LIMIT 20$q$),
 (7, $q$SELECT dm.grp, count(*) FROM ev LEFT JOIN dm ON dm.k = ev.k AND dm.d > date '2026-06-01' GROUP BY 1 ORDER BY 1$q$),
 (9, $q$SELECT dp.grp, min(dp.ts), max(dp.tz), max(dp.i8), sum(dp.n), count(dp.b), min(dp.t), max(dp.vc) FROM ev JOIN dp ON dp.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (10, $q$SELECT dp.n, count(*) FROM ev JOIN dp ON dp.k = ev.k AND dp.i8 > 100000000 GROUP BY 1 ORDER BY 1 LIMIT 20$q$),
 (8, $q$SELECT dn.grp, count(*), sum(ev.v) FROM ev JOIN dn ON dn.k = ev.k GROUP BY 1 ORDER BY 1$q$);

-- the rows of the answer, and whether SiftAggV6 joins the table
CREATE FUNCTION lookup(sql text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ 'Lookup Joins: ' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION report(stage text) RETURNS TABLE (stage text, shape int, same_answers bool, lookup bool, not_empty bool) LANGUAGE sql AS $$
	SELECT stage, n, sift_helpers.answer(sql, true) = sift_helpers.answer(sql, false), lookup(sql), sift_helpers.answer(sql, true) <> '' FROM shapes ORDER BY n
$$;

SELECT * FROM report('settled');

-- rows the heap changed after the index saw them: updated, deleted, inserted (the write buffer), in the transaction that reads them
BEGIN;
UPDATE dm SET grp = 6, t = 'kind1', n = 1.25, d = date '2026-12-01' WHERE k % 31 = 0;
DELETE FROM dm WHERE k % 53 = 0;
UPDATE dp SET grp = 6, t = 'kind1', n = 1.25, d = date '2026-12-01' WHERE k % 31 = 0;
DELETE FROM dp WHERE k % 53 = 0;
INSERT INTO dm SELECT i, 'kind' || i % 9, 'v1', date '2026-01-01' + i % 90, timestamp '2026-01-05', timestamptz '2026-01-05+00', 4, i, true, 2.5, i % 7 FROM generate_series(3200, 3299) i;
INSERT INTO ev SELECT i, 3200 + i % 100, 1 FROM generate_series(40001, 40500) i;
SELECT * FROM report('in the transaction');
ROLLBACK;

-- committed, not vacuumed: pages that are not all-visible
UPDATE dm SET n = n + 1 WHERE k % 29 = 0;
DELETE FROM dm WHERE k % 61 = 0;
UPDATE dp SET n = n + 1 WHERE k % 29 = 0;
DELETE FROM dp WHERE k % 61 = 0;
SELECT * FROM report('committed, not vacuumed');
VACUUM dm, dp;
SELECT * FROM report('vacuumed');

DROP FUNCTION report(text), lookup(text);
DROP TABLE shapes, ev, dm, dn, dp;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
