-- A filter on a dimension of the cube (`kind = 3`, `kind IN (1, 4)`, a range) with no group key is answered from the histogram of that dimension: the
-- groups that pass the filter are added up for the units whose zone is not inside it, the sums of the leaves for those whose zone is.
-- Every query runs three ways, which must give the same rows (text, so the scale of the numeric counts): from the pre-aggregates, scanning
-- every unit (sift.agg_preagg off), and the plain plan (sift.enable_agg off). `pre` says how many units the first one answered from metadata.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
-- workers divide the heap blocks among themselves as they run, so a parallel build lays the rows out in other units from run to run, and the counts of
-- units answered from the pre-aggregates follow the layout: every build is serial except the one of step 6
SET max_parallel_maintenance_workers = 0;

\ir test/agg_preagg_helpers.sql

\ir test/agg_preagg_units.sql

CREATE TABLE ev (id int, kind int, big bigint, grp text, val int, small int, amt numeric(12, 2), pad text) WITH (autovacuum_enabled = off);

CREATE TABLE pa_q (n int, shape text, q text);
INSERT INTO pa_q VALUES
	(1, 'eq, count',          'SELECT count(*) FROM %s WHERE kind = 3'),
	(2, 'eq, sums',           'SELECT count(*), sum(val), avg(val), count(val), sum(amt), avg(amt), count(amt) FROM %s WHERE kind = 3'),
	(3, 'eq 0, the NULL group', 'SELECT count(*), sum(val) FROM %s WHERE kind = 0'),
	(4, 'eq, absent',         'SELECT count(*), sum(val), avg(amt) FROM %s WHERE kind = 99'),
	(5, 'eq, negative',       'SELECT count(*), sum(val) FROM %s WHERE kind = -2'),
	(6, 'in list',            'SELECT count(*), sum(val) FROM %s WHERE kind IN (1, 3)'),
	(7, 'in list, gaps',      'SELECT count(*), sum(val), sum(amt) FROM %s WHERE kind IN (1, 4, 6)'),
	(8, 'in list, NULL',      'SELECT count(*), sum(val) FROM %s WHERE kind IN (2, NULL)'),
	(9, 'in list, absent',    'SELECT count(*), sum(val) FROM %s WHERE kind IN (98, 99)'),
	(10, 'in list, one',      'SELECT count(*), sum(val) FROM %s WHERE kind IN (5)'),
	(11, 'range',             'SELECT count(*), sum(val), avg(amt) FROM %s WHERE kind >= 4'),
	(12, 'range, both ends',  'SELECT count(*), sum(val) FROM %s WHERE kind > 1 AND kind < 5'),
	(13, 'not equal',         'SELECT count(*), sum(val) FROM %s WHERE kind <> 3'),
	(14, 'IS NULL',           'SELECT count(*), sum(val), count(kind) FROM %s WHERE kind IS NULL'),
	(15, 'count of the dim',  'SELECT count(kind), count(*) FROM %s WHERE kind = 3'),
	(16, 'eq, zone on the id', 'SELECT count(*), sum(val) FROM %s WHERE kind = 3 AND id < 20000'),
	(17, 'eq, zone covers',   'SELECT count(*), sum(val) FROM %s WHERE kind = 3 AND id >= 1'),
	(18, 'two dims: scan',    'SELECT count(*), sum(val) FROM %s WHERE kind = 3 AND big = 4611686018427387905'),
	(19, 'int8 eq',           'SELECT count(*), sum(val) FROM %s WHERE big = 4611686018427387905'),
	(20, 'int8 in list',      'SELECT count(*), sum(val), sum(amt) FROM %s WHERE big IN (4611686018427387905, -4611686018427387905, 0)'),
	(21, 'int8 extremes',     'SELECT count(*), sum(val) FROM %s WHERE big IN (9223372036854775807, -9223372036854775807 - 1)'),
	(22, 'int8 range',        'SELECT count(*), sum(val) FROM %s WHERE big >= 0'),
	(23, 'int8 not equal',    'SELECT count(*), sum(val) FROM %s WHERE big <> 4611686018427387905'),
	(24, 'int8 IS NULL',      'SELECT count(*), sum(val) FROM %s WHERE big IS NULL'),
	(25, 'text eq',           'SELECT count(*), sum(val) FROM %s WHERE grp = ''g05'''),
	(26, 'min, max: scan',    'SELECT min(val), max(val), count(*) FROM %s WHERE kind = 3'),
	(27, 'measure not stored', 'SELECT sum(small) FROM %s WHERE kind = 3'),
	(28, 'other key: scan',   'SELECT grp, count(*) FROM %s WHERE kind = 3 GROUP BY grp'),
	(29, 'the key is the dim', 'SELECT kind, count(*), sum(val) FROM %s WHERE kind = 3 GROUP BY kind'),
	(30, 'FILTER on the dim',      'SELECT count(*) FILTER (WHERE kind = 3) FROM %s'),
	(31, 'no rows',           'SELECT count(*), sum(val) FROM %s WHERE kind = 3 AND id > 100000000'),
	(32, 'the dim and its text', 'SELECT count(*), sum(val) FROM %s WHERE kind = 3 AND grp = ''g05''');

-- one row per query: whether the three ways agree, and how much the first answered from metadata
CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 32) RETURNS TABLE (n int, shape text, pre text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.pre IS NULL THEN '-' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END,
	       pa_rows(z.q, true, true) IS NOT DISTINCT FROM pa_rows(z.q, false, true) AND
	       pa_rows(z.q, false, true) IS NOT DISTINCT FROM pa_rows(z.q, false, false)
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pa_q WHERE n BETWEEN lo AND hi) z, LATERAL pa_units(z.q) u ORDER BY z.n
$$;

-- the same, summed up for a table whose answers need not be listed
CREATE FUNCTION pa_sum(tbl text, lo int DEFAULT 1, hi int DEFAULT 32) RETURNS TABLE (queries bigint, differ bigint, all_pre bigint, some_pre bigint, no_pre bigint) LANGUAGE plpgsql AS $$
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

-- kind: 0 to 6 (a ninth of the rows NULL, -2 for one row) in every unit, so no zone lies inside a comparison with it; big: a few int8 values with the
-- extremes and NULLs, the sign and 2^62 as dims; val and amt: NULLs among the values
INSERT INTO ev
SELECT g, CASE WHEN g % 9 = 0 THEN NULL WHEN g = 1000 THEN -2 ELSE g % 7 END,
       CASE g % 6 WHEN 0 THEN 4611686018427387905 WHEN 1 THEN -4611686018427387905 WHEN 2 THEN 0 WHEN 3 THEN NULL
                  WHEN 4 THEN CASE WHEN g % 12 = 4 THEN 9223372036854775807 ELSE -9223372036854775807 - 1 END ELSE 7 END,
       CASE WHEN g % 17 = 0 THEN NULL ELSE 'g' || lpad((g % 30)::text, 2, '0') END,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 37) % 1000 - 400 END, g % 13,
       CASE WHEN g % 19 = 0 THEN NULL ELSE ((g::bigint * 37) % 100000)::numeric / 100 END, repeat('p', 200)
FROM generate_series(1, 40000) g;

-- an explicit cube with the measures; the default cube; no cube
CREATE TABLE ec (LIKE ev) WITH (autovacuum_enabled = off);
CREATE TABLE ed (LIKE ev) WITH (autovacuum_enabled = off);
CREATE TABLE eo (LIKE ev) WITH (autovacuum_enabled = off);
INSERT INTO ec SELECT * FROM ev;
INSERT INTO ed SELECT * FROM ev;
INSERT INTO eo SELECT * FROM ev;
DROP TABLE ev;
CREATE INDEX ec_col ON ec USING sift_col (id, kind, big, grp, val, small, amt)
	WITH (write_buffer_limit = 4096, preagg = 'cube(kind: val, amt; big: val, amt; grp: val)');
CREATE INDEX ed_col ON ed USING sift_col (id, kind, big, grp, val, small, amt) WITH (write_buffer_limit = 4096);
CREATE INDEX eo_col ON eo USING sift_col (id, kind, big, grp, val, small, amt) WITH (write_buffer_limit = 4096, preagg = 'off');
CALL pa_settle();
-- FREEZE: a plain VACUUM skips a page another backend has pinned (the checkpointer or the background writer writing it) and leaves it not
-- all-visible, so its unit is scanned; one that has to freeze a page waits for the cleanup lock, and every page written since the load has tuples to freeze
VACUUM (ANALYZE, FREEZE, DISABLE_PAGE_SKIPPING) ec, ed, eo;
SELECT sift.col_verify('ec_col', true);

-- 1. built, every page all-visible
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');
SELECT * FROM pa_sum('eo');

-- the answers of the first way, which the plain plan gives too (the rows of a query on one line)
SELECT n, replace(pa_rows(format(q, 'ec'), true, true), E'\n', ' / ') AS answer FROM pa_q WHERE n <> 28 ORDER BY n;

-- the plan counts the units answered from the pre-aggregates
SELECT units > 1 AS units, pre = units AS all_from_preagg FROM pa_units('SELECT count(*), sum(val) FROM ec WHERE kind IN (1, 4, 6)');

-- 2. a few pages change (update, delete): their units are scanned, the others answer
UPDATE ec SET val = -(val / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 100 AND 200;
DELETE FROM ec WHERE id BETWEEN 18000 AND 18100;
UPDATE ed SET val = -(val / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 100 AND 200;
DELETE FROM ed WHERE id BETWEEN 18000 AND 18100;
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');

-- 3. vacuumed: the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec, ed;
SELECT * FROM pa_check('ec');

-- 4. rows in the write buffer (a kind and a big no unit holds), then sealed
INSERT INTO ec SELECT g, CASE WHEN g % 4 = 0 THEN NULL ELSE 7 + g % 3 END, CASE WHEN g % 3 = 0 THEN 4611686018427387905 ELSE 5 END,
                  'g' || (g % 40), g % 9, g % 5, g % 100, repeat('p', 200)
FROM generate_series(300001, 301500) g;
SELECT * FROM pa_check('ec');
SELECT count(*), sum(val) FROM ec WHERE kind = 8;
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
FROM (SELECT n, pa_units(format(q, 'ec')) AS u FROM pa_q WHERE n IN (1, 7, 19)) z ORDER BY n;
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
SELECT pre IS NULL AS off FROM pa_units('SELECT count(*) FROM ed WHERE kind = 3');
RESET sift.agg_preagg;
SELECT pre > 0 AS on FROM pa_units('SELECT count(*) FROM ed WHERE kind = 3');

DROP TABLE ec, ed, eo, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int);
DROP PROCEDURE pa_settle();
DROP EXTENSION pg_sift;
