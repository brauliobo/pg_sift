-- sum(int8) and avg(int8) are numeric aggregates over an exact 128-bit sum: the units that are exact answer them from the stored sums.
-- Every query runs three ways, which must give the same rows (text, so the scale of the numeric counts): from the pre-aggregates, scanning
-- every unit (sift.agg_preagg off), and the plain plan (sift.enable_agg off). `pre` says how many units the first one answered from metadata.
CREATE EXTENSION pg_sift;
SET sift.agg_dead_delta = 0;	-- a unit with a dead row is scanned here (col_dead_delta answers it net of the dead rows)
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
-- workers divide the heap blocks among themselves as they run, so a parallel build lays the rows out in other units from run to run, and the counts of
-- units answered from the pre-aggregates follow the layout: every build is serial except the one of step 6
SET max_parallel_maintenance_workers = 0;

\ir test/agg_preagg_helpers.sql

\ir test/agg_preagg_units.sql

CREATE TABLE ev (id int, kind int, grp text, val bigint, small bigint, pad text) WITH (autovacuum_enabled = off);

CREATE TABLE pa_q (n int, shape text, q text);
INSERT INTO pa_q VALUES
	(1, 'whole',               'SELECT sum(val), avg(val), count(val), count(*) FROM %s'),
	(2, 'whole, small values', 'SELECT sum(small), avg(small) FROM %s'),
	(3, 'by kind',             'SELECT kind, sum(val), avg(val), sum(small), avg(small) FROM %s GROUP BY kind'),
	(4, 'by text key',         'SELECT grp, sum(val), avg(val) FROM %s GROUP BY grp'),
	(5, 'only NULLs',          'SELECT sum(val), avg(val), count(val), count(*) FROM %s WHERE kind = 3'),
	(6, 'no rows',             'SELECT sum(val), avg(val), count(*) FROM %s WHERE id > 100000000'),
	(7, 'no rows, grouped',    'SELECT kind, sum(val), avg(val) FROM %s WHERE id > 100000000 GROUP BY kind'),
	(8, 'zone covers all',     'SELECT sum(val), avg(val) FROM %s WHERE id >= 1'),
	(9, 'zone covers some',    'SELECT sum(val), avg(val) FROM %s WHERE id < 20000'),
	(10, 'keys in list',       'SELECT kind, sum(val), avg(val) FROM %s WHERE kind IN (0, 1, 3) GROUP BY kind'),
	(11, 'test of dim',        'SELECT sum(val), avg(val) FROM %s WHERE grp = ''g05'''),
	(12, 'other test: scan',   'SELECT kind, sum(val), avg(val) FROM %s WHERE small > 0 GROUP BY kind'),
	(13, 'with min and max',   'SELECT kind, sum(val), avg(val), min(val), max(val) FROM %s GROUP BY kind'),
	(14, 'grouping sets',      'SELECT kind, grp, sum(val), avg(val) FROM %s GROUP BY GROUPING SETS ((kind), (grp), ())'),
	(15, 'rollup',             'SELECT kind, sum(val), avg(val) FROM %s GROUP BY ROLLUP (kind)'),
	(16, 'FILTER on the dim',       'SELECT sum(val) FILTER (WHERE kind = 1), avg(val) FILTER (WHERE kind = 1) FROM %s'),
	(17, 'DISTINCT, expr: scan', 'SELECT sum(DISTINCT val), avg(DISTINCT val), sum(val + 1), avg(val * 2) FROM %s'),
	(18, 'having, limit',      'SELECT kind, sum(val) FROM %s GROUP BY kind HAVING sum(val) > 0 ORDER BY 2 DESC, 1 LIMIT 3'),
	(19, 'int4 beside int8',   'SELECT kind, sum(kind), avg(kind), sum(val) FROM %s GROUP BY kind');

-- one row per query: whether the three ways agree, and how much the first answered from metadata
CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 19) RETURNS TABLE (n int, shape text, pre text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.pre IS NULL THEN '-' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END,
	       pa_rows(z.q, true, true) IS NOT DISTINCT FROM pa_rows(z.q, false, true) AND
	       pa_rows(z.q, false, true) IS NOT DISTINCT FROM pa_rows(z.q, false, false)
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pa_q WHERE n BETWEEN lo AND hi) z, LATERAL pa_units(z.q) u ORDER BY z.n
$$;

-- the same, summed up for a table whose answers need not be listed
CREATE FUNCTION pa_sum(tbl text, lo int DEFAULT 1, hi int DEFAULT 19) RETURNS TABLE (queries bigint, differ bigint, all_pre bigint, some_pre bigint, no_pre bigint) LANGUAGE plpgsql AS $$
DECLARE state text;
BEGIN
	SELECT count(*), count(*) FILTER (WHERE NOT same), count(*) FILTER (WHERE pre = 'all'), count(*) FILTER (WHERE pre = 'some'),
	       count(*) FILTER (WHERE pre = 'none'), string_agg(n || ':' || pre, ' ' ORDER BY n)
	INTO queries, differ, all_pre, some_pre, no_pre, state FROM pa_check(tbl, lo, hi);
	-- the state of the run goes to the server log (kept by CI), not to the output: a count that differs from the expected one can be read there
	RAISE LOG 'pa_sum %: % | heap all-visible % | units exact % | queue % | snapshots %', tbl, state,
		(SELECT relallvisible || '/' || relpages FROM pg_class WHERE oid = tbl::regclass),
		(SELECT string_agg(segment || '.' || sb || '=' || exact::int, ' ' ORDER BY segment, sb) FROM sift.col_unit_agg((tbl || '_col')::regclass, 'id')),
		(SELECT count(*) FROM sift.col_rollup_queue()),
		(SELECT string_agg(backend_type || ' ' || backend_xmin, ', ') FROM pg_stat_activity
		 WHERE datname = current_database() AND pid <> pg_backend_pid() AND backend_xmin IS NOT NULL);
	RETURN NEXT;
END $$;

-- kind 0: near 2^62 (two rows pass 2^63), kind 1: near -2^62, kind 2: the extremes of int8 with both signs, kind 3: only NULLs,
-- kind 4: small values, a fifth of them NULL, kind 5: values that make the average a repeating decimal
INSERT INTO ev
SELECT g, g % 6, CASE WHEN g % 17 = 0 THEN NULL ELSE 'g' || lpad((g % 30)::text, 2, '0') END,
       CASE g % 6 WHEN 0 THEN 4611686018427387904 - g % 1000
                  WHEN 1 THEN -4611686018427387904 + g % 1000
                  WHEN 2 THEN CASE WHEN g % 12 = 2 THEN 9223372036854775807 - g % 7 ELSE -9223372036854775807 - 1 + g % 7 END
                  WHEN 3 THEN NULL
                  WHEN 4 THEN CASE WHEN g % 5 = 0 THEN NULL ELSE g % 100 - 50 END
                  ELSE (g % 997) * 3 + 1 END,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 37) % 1000 - 400 END, repeat('p', 200)
FROM generate_series(1, 40000) g;

-- an explicit cube with the measures; the default cube; no cube
CREATE TABLE ec (LIKE ev) WITH (autovacuum_enabled = off);
CREATE TABLE ed (LIKE ev) WITH (autovacuum_enabled = off);
CREATE TABLE eo (LIKE ev) WITH (autovacuum_enabled = off);
INSERT INTO ec SELECT * FROM ev;
INSERT INTO ed SELECT * FROM ev;
INSERT INTO eo SELECT * FROM ev;
DROP TABLE ev;
CREATE INDEX ec_col ON ec USING sift_col (id, kind, grp, val, small)
	WITH (write_buffer_limit = 4096, preagg = 'cube(kind: val, small; grp: val, small)');
CREATE INDEX ed_col ON ed USING sift_col (id, kind, grp, val, small) WITH (write_buffer_limit = 4096);
CREATE INDEX eo_col ON eo USING sift_col (id, kind, grp, val, small) WITH (write_buffer_limit = 4096, preagg = 'off');
CALL pa_settle();
-- FREEZE: a page whose buffer another backend has pinned (the checkpointer writing it) is passed by a VACUUM that has nothing to freeze there,
-- and stays out of the visibility map, so its unit is scanned; with a tuple to freeze on every page the VACUUM waits for the pin
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ec, ed, eo;
SELECT sift.col_verify('ec_col', true);

-- the result is PostgreSQL's: a numeric of scale 0 for the sum, the numeric division of the sum by the count for the average
SELECT kind, pg_typeof(sum(val)) AS sum_type, sum(val), scale(avg(val)) AS avg_scale, avg(val) FROM ec GROUP BY kind ORDER BY kind;
SELECT kind, sum(val), avg(val) FROM ec GROUP BY kind ORDER BY kind;
SET sift.enable_agg = off;
SELECT kind, sum(val), avg(val) FROM ec GROUP BY kind ORDER BY kind;
RESET sift.enable_agg;
SELECT (SELECT sum(val) FROM ec WHERE kind = 0) > 9223372036854775807 AS sum_passes_int8;
SELECT sum(val) = (SELECT sum(val) FROM eo) AS same_sum FROM ec;

-- 1. built, every page all-visible
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');
SELECT * FROM pa_sum('eo');

-- 2. a few pages change (update, delete): their units are scanned, the others answer
UPDATE ec SET val = -(val / 2), small = small + 1 WHERE id BETWEEN 100 AND 200;
DELETE FROM ec WHERE id BETWEEN 18000 AND 18100;
UPDATE ed SET val = -(val / 2), small = small + 1 WHERE id BETWEEN 100 AND 200;
DELETE FROM ed WHERE id BETWEEN 18000 AND 18100;
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');

-- 3. vacuumed: the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec, ed;
SELECT * FROM pa_check('ec');

-- 4. rows in the write buffer, then sealed
INSERT INTO ec SELECT g, g % 6, 'g' || (g % 40), CASE WHEN g % 6 = 3 THEN NULL ELSE 4611686018427387904 + g END, g % 9, repeat('p', 200)
FROM generate_series(300001, 301500) g;
SELECT * FROM pa_check('ec');
SELECT sift.col_promote('ec_col') >= 0 AS promoted;
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec;
SELECT * FROM pa_check('ec');

-- 5. merged: the dead rows are gone and every unit is stored with its pre-aggregates again
SELECT sift.col_merge('ec_col', true) >= 0 AS merged;
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec;
SELECT sift.col_verify('ec_col', true);
SELECT * FROM pa_check('ec');

-- 6. built by parallel workers (several segments), then read by the workers of the scan: each folds its share and the states merge
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_leader_participation = off;
REINDEX INDEX ec_col;
RESET parallel_leader_participation;
SELECT segments > 1 AS parallel_segments FROM sift.col_index_info('ec_col');
-- the layout of a parallel build is not the same twice: its answers must be, the counts are for a serial rebuild
SELECT count(*) FILTER (WHERE NOT same) AS differ FROM pa_check('ec');
SET max_parallel_maintenance_workers = 0;
REINDEX INDEX ec_col;
SELECT * FROM pa_check('ec');
UPDATE ec SET small = small WHERE id < 20000;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('ec');
SELECT n, (u).workers > 0 AS workers, (u).pre > 0 AS some_pre
FROM (SELECT n, pa_units(format(q, 'ec')) AS u FROM pa_q WHERE n IN (1, 3)) z ORDER BY n;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;

-- a transaction that deleted rows sees them gone, another does not until it commits
BEGIN;
DELETE FROM eo WHERE id BETWEEN 1 AND 100;
SELECT * FROM pa_sum('eo');
ROLLBACK;
SELECT * FROM pa_sum('eo');

-- the switch
SET sift.agg_preagg = off;
SELECT pre IS NULL AS off FROM pa_units('SELECT sum(val), avg(val) FROM ed');
RESET sift.agg_preagg;
SELECT pre > 0 AS on FROM pa_units('SELECT sum(val), avg(val) FROM ed');

DROP TABLE ec, ed, eo, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int);
DROP PROCEDURE pa_settle();
DROP EXTENSION pg_sift;
