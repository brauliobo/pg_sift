-- A unit that has a rollup is answered from it only when its tuples cost less than its elements (sift.agg_rollup_cost scales the cost of a tuple, 0: always the
-- rollup). Two tables hold the same documents: ro_lo has a rollup of one dim (a few tuples a unit against thousands of elements), ro_hi a rollup of three
-- dims whose tuples are a large share of the elements. The answers never change; the units answered from the rollups and the units scanned by cost do.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
-- workers divide the heap blocks among themselves as they run, so a parallel build lays the rows out in other units from run to run, and the units answered
-- from the rollups or by cost follow the layout
SET max_parallel_maintenance_workers = 0;
SET cpu_tuple_cost = 0.5;
SET sift.col_rollup_worker = off;
SET sift.col_rollup_on_demand = off;

CREATE TABLE ro_lo (id int, items jsonb, pad text) WITH (autovacuum_enabled = off);
INSERT INTO ro_lo
SELECT g, (SELECT jsonb_agg(jsonb_build_object('kind', 'k' || (g * e) % 40, 'band', 1 + (g * 7 + e * 3) % 10, 'seen', 'm' || (g + e) % 6,
                                                'score', ((g * e) % 40) * 0.25, 'qty', (g * e) % 9) ORDER BY e)
           FROM generate_series(1, g % 5 + 1) e), repeat('p', 200)
FROM generate_series(1, 12000) g;
CREATE TABLE ro_hi (LIKE ro_lo) WITH (autovacuum_enabled = off);
INSERT INTO ro_hi SELECT * FROM ro_lo;
CREATE INDEX ro_lo_col ON ro_lo USING sift_col (id, items) WITH (paths = 'items[]{kind text, band int4, seen text, score float8, qty int4}', rollup = 'items[].kind');
CREATE INDEX ro_hi_col ON ro_hi USING sift_col (id, items) WITH (paths = 'items[]{kind text, band int4, seen text, score float8, qty int4}', rollup = 'items[].kind, items[].band, items[].seen');
-- wide rows make many units (a unit is 256 pages): after a few units scanned by cost the next ones are scanned without reading their rollups, and the answers and counts stay
CREATE TABLE ro_big (LIKE ro_lo) WITH (autovacuum_enabled = off);
ALTER TABLE ro_big ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ro_big SELECT id + 12000 * k, items, repeat('p', 1500) FROM ro_lo, generate_series(0, 2) k;
CREATE INDEX ro_big_col ON ro_big USING sift_col (id, items) WITH (paths = 'items[]{kind text, band int4, seen text, score float8, qty int4}', rollup = 'items[].kind, items[].band, items[].seen');
VACUUM (ANALYZE, FREEZE, DISABLE_PAGE_SKIPPING) ro_lo, ro_hi, ro_big;
SELECT units > 20 AS many_units FROM sift.col_index_info('ro_big_col');

CREATE FUNCTION rc_rows(q text, cost text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.agg_rollup_cost', cost, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- units, units answered from the rollups, units scanned because their tuples cost more
CREATE FUNCTION rc_units(q text, cost text, OUT units int, OUT pre int, OUT by_cost int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.agg_rollup_cost', cost, true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from '(\d+) from pre-aggregates')::int, pre);
		by_cost := coalesce(substring(l from '(\d+) scanned by cost')::int, by_cost);
	END LOOP;
END $$;

CREATE TABLE rc_q (n int, shape text, q text);
INSERT INTO rc_q VALUES
	(1, 'one key',         'SELECT e.kind, count(*), sum(e.qty), avg(e.score) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(2, 'one key, filter', 'SELECT e.kind, count(*) FILTER (WHERE e.kind <> ''k1''), min(e.score) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(3, 'counts only',     'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, band int4, seen text, score float8, qty int4) GROUP BY 1');

-- the default cost, no cost (always the rollup) and a cost that scans every unit: the same rows
SELECT t.tbl, q.n, q.shape, rc_rows(format(q.q, t.tbl), '1') = rc_rows(format(q.q, t.tbl), '0') AS same_always, rc_rows(format(q.q, t.tbl), '1') = rc_rows(format(q.q, t.tbl), '1000') AS same_never
FROM (VALUES ('ro_lo'), ('ro_hi'), ('ro_big')) t(tbl), rc_q q ORDER BY 1, 2;

-- how the units were answered at the default cost: from the rollups, by cost, or both
CREATE FUNCTION rc_how(tbl text, cost text, lo int DEFAULT 1, hi int DEFAULT 99) RETURNS TABLE (n int, how text) LANGUAGE sql AS $$
	SELECT q.n, CASE WHEN u.pre = u.units THEN 'rollups' WHEN u.by_cost = u.units THEN 'scanned by cost' WHEN u.pre > 0 AND u.by_cost > 0 THEN 'both' ELSE 'other' END
	FROM rc_q q, LATERAL rc_units(format(q.q, tbl), cost) u WHERE q.n BETWEEN lo AND hi ORDER BY q.n
$$;
SELECT 'ro_lo' AS tbl, * FROM rc_how('ro_lo', '1');
SELECT 'ro_hi' AS tbl, * FROM rc_how('ro_hi', '1');
SELECT 'ro_big' AS tbl, * FROM rc_how('ro_big', '1');
-- no cost: the rollups answer every unit; a cost that is never worth it: every unit scans
SELECT 'ro_hi' AS tbl, * FROM rc_how('ro_hi', '0');
SELECT 'ro_lo' AS tbl, * FROM rc_how('ro_lo', '1000');

DROP TABLE ro_lo, ro_hi, ro_big, rc_q;
DROP FUNCTION rc_rows(text, text), rc_units(text, text), rc_how(text, text, int, int);
DROP EXTENSION pg_sift;
