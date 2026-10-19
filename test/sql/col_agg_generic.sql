-- A generic plan (a prepared statement after its custom plans, or plan_cache_mode = force_generic_plan) guesses the selectivity of a
-- comparison with a bound parameter: a narrow guess made the scan over the unnested elements look dearer than the normal plan, which
-- reads every row of the heap whatever the parameter holds. The scan is chosen as with literals, and the rows are the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE g_t (id int PRIMARY KEY, grp int, created date, kind text, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO g_t
SELECT i, 1 + i % 200, date '2024-01-01' + (i % 900), (ARRAY['A', 'b', 'C'])[1 + i % 3],
       jsonb_build_object('lines', (SELECT jsonb_agg(jsonb_build_object('k', (ARRAY['a', 'b', 'c'])[1 + (i + j) % 3], 'n', (i + j) % 10)) FROM generate_series(1, 1 + i % 4) j))
FROM generate_series(1, 60000) i;
CREATE INDEX g_t_col ON g_t USING sift_col (id, grp, created, kind, doc) WITH (paths = 'doc.lines[]{k text, n float8}');
VACUUM (FREEZE, ANALYZE) g_t;

CREATE FUNCTION plan_uses_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

SET plan_cache_mode = force_generic_plan;
PREPARE arr(int[], date, date) AS
SELECT t.kind, count(DISTINCT t.id) AS n, count(*) AS c, sum(l.n) AS s FROM g_t t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'lines') AS l(k text, n float8)
WHERE t.grp = ANY($1) AND t.created >= $2 AND t.created <= $3 GROUP BY t.kind;
PREPARE dates(date, date) AS
SELECT l.k, count(*) AS c, sum(l.n) AS s FROM g_t t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'lines') AS l(k text, n float8)
WHERE t.created >= $1 AND t.created <= $2 GROUP BY l.k;
PREPARE one(int) AS
SELECT t.kind, count(*) AS c, sum(l.n) AS s FROM g_t t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'lines') AS l(k text, n float8) WHERE t.id = $1 GROUP BY t.kind;

-- the scan, and the same rows as the normal plan
SELECT plan_uses_scan($$EXECUTE arr('{1,2,3,4,5,6,7,8,9,10}', '2024-01-01', '2026-06-30')$$) AS arr_scan;
SELECT plan_uses_scan($$EXECUTE dates('2024-03-01', '2025-09-30')$$) AS dates_scan;
CREATE TEMP TABLE r_on AS EXECUTE arr('{1,2,3,4,5,6,7,8,9,10}', '2024-01-01', '2026-06-30');
CREATE TEMP TABLE r_dates AS EXECUTE dates('2024-03-01', '2025-09-30');
SET sift.enable_agg = off;
CREATE TEMP TABLE r_off AS EXECUTE arr('{1,2,3,4,5,6,7,8,9,10}', '2024-01-01', '2026-06-30');
CREATE TEMP TABLE r_dates_off AS EXECUTE dates('2024-03-01', '2025-09-30');
RESET sift.enable_agg;
SELECT count(*) AS arr_diff FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
SELECT count(*) AS dates_diff FROM ((TABLE r_dates EXCEPT TABLE r_dates_off) UNION ALL (TABLE r_dates_off EXCEPT TABLE r_dates)) d;
SELECT count(*) > 0 AS has_rows FROM r_on;

-- a lookup by the primary key keeps the index scan: the bound parameter is as selective as the estimate says
SELECT plan_uses_scan($$EXECUTE one(1234)$$) AS one_scan;

DEALLOCATE ALL;
DROP TABLE g_t;
DROP FUNCTION plan_uses_scan(text);
DROP EXTENSION pg_sift;
