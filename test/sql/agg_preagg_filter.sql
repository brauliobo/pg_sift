-- A FILTER on the dimension of the cube that a query groups or filters by (`count(*) FILTER (WHERE kind = 3)`, `sum(val) FILTER (WHERE kind IN (1, 4))`) is
-- answered from the histogram of that dimension: the groups that pass the FILTER add to the aggregate, the others add nothing to it.
-- Every query runs three ways, which must give the same rows (text, so the scale of the numeric counts): from the pre-aggregates, scanning
-- every unit (sift.agg_preagg off), and the plain plan (sift.enable_agg off). `pre` says how many units the first one answered from metadata.
CREATE EXTENSION pg_sift;
SET default_statistics_target = 10000;
SET sift.agg_dead_delta = 0;	-- a unit with a dead row is scanned here (col_dead_delta answers it net of the dead rows)
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

\ir test/agg_preagg_helpers.sql

\ir test/agg_preagg_units.sql

CREATE TABLE ev (id int, kind int, big bigint, grp text, val int, small int, amt numeric(12, 2), pad text) WITH (autovacuum_enabled = off);

CREATE TABLE pa_q (n int, shape text, q text);
INSERT INTO pa_q VALUES
	(1, 'count',              'SELECT count(*) FILTER (WHERE kind = 3) FROM %s'),
	(2, 'count and total',    'SELECT count(*), count(*) FILTER (WHERE kind = 3) FROM %s'),
	(3, 'sums',               'SELECT sum(val) FILTER (WHERE kind = 3), avg(val) FILTER (WHERE kind = 3), count(val) FILTER (WHERE kind = 3), sum(amt) FILTER (WHERE kind = 3), avg(amt) FILTER (WHERE kind = 3), count(amt) FILTER (WHERE kind = 3) FROM %s'),
	(4, 'two filters',        'SELECT count(*) FILTER (WHERE kind = 1), count(*) FILTER (WHERE kind = 2), sum(val) FILTER (WHERE kind IN (3, 4)), sum(val), count(*) FROM %s'),
	(5, 'zero, the NULL group', 'SELECT count(*) FILTER (WHERE kind = 0), sum(val) FILTER (WHERE kind = 0) FROM %s'),
	(6, 'absent',             'SELECT count(*) FILTER (WHERE kind = 99), sum(val) FILTER (WHERE kind = 99), avg(amt) FILTER (WHERE kind = 99) FROM %s'),
	(7, 'down',           'SELECT count(*) FILTER (WHERE kind = -2), sum(val) FILTER (WHERE kind = -2) FROM %s'),
	(8, 'in list, gaps',      'SELECT count(*) FILTER (WHERE kind IN (1, 4, 6)), sum(val) FILTER (WHERE kind IN (1, 4, 6)) FROM %s'),
	(9, 'in list, NULL',      'SELECT count(*) FILTER (WHERE kind IN (2, NULL)), sum(val) FILTER (WHERE kind IN (2, NULL)) FROM %s'),
	(10, 'range',             'SELECT count(*) FILTER (WHERE kind >= 4), sum(val) FILTER (WHERE kind > 1 AND kind < 5) FROM %s'),
	(11, 'not equal',         'SELECT count(*) FILTER (WHERE kind <> 3), sum(val) FILTER (WHERE kind <> 3) FROM %s'),
	(12, 'IS NULL',           'SELECT count(*) FILTER (WHERE kind IS NULL), sum(val) FILTER (WHERE kind IS NULL), count(kind) FILTER (WHERE kind IS NULL) FROM %s'),
	(13, 'OR on the dim',     'SELECT count(*) FILTER (WHERE kind = 1 OR kind = 5), sum(val) FILTER (WHERE kind = 1 OR kind IS NULL) FROM %s'),
	(14, 'count of the dim',  'SELECT count(kind) FILTER (WHERE kind = 3), count(kind) FILTER (WHERE kind > 2), count(kind) FROM %s'),
	(15, 'key is the dim',    'SELECT kind, count(*), count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind > 2), sum(val) FROM %s GROUP BY kind'),
	(16, 'key, a group never passes', 'SELECT kind, count(*) FILTER (WHERE kind = 99), sum(val) FILTER (WHERE kind = 99), avg(amt) FILTER (WHERE kind = 99) FROM %s GROUP BY kind'),
	(17, 'text key',          'SELECT grp, count(*) FILTER (WHERE grp = ''g05''), sum(val) FILTER (WHERE grp IN (''g05'', ''g06'')), count(*) FROM %s GROUP BY grp'),
	(18, 'where on the dim',  'SELECT count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind > 1), sum(val) FROM %s WHERE kind <> 2'),
	(19, 'key and where',     'SELECT kind, count(*) FILTER (WHERE kind IN (1, 3)), sum(val) FILTER (WHERE kind <> 4) FROM %s WHERE kind > 0 GROUP BY kind'),
	(20, 'zone on the id',    'SELECT count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind = 3) FROM %s WHERE id < 20000'),
	(21, 'zone covers',       'SELECT count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind = 3) FROM %s WHERE id >= 1'),
	(22, 'no rows',           'SELECT count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind = 3) FROM %s WHERE id > 100000000'),
	(23, 'int8 dim',          'SELECT count(*) FILTER (WHERE big = 4611686018427387905), sum(val) FILTER (WHERE big IN (0, 7)), sum(amt) FILTER (WHERE big IS NULL) FROM %s'),
	(24, 'int8 extremes',     'SELECT count(*) FILTER (WHERE big IN (9223372036854775807, -9223372036854775807 - 1)), sum(val) FILTER (WHERE big < 0) FROM %s'),
	(25, 'text dim',          'SELECT count(*) FILTER (WHERE grp = ''g05''), sum(val) FILTER (WHERE grp = ''g05''), count(*) FILTER (WHERE grp IS NULL) FROM %s'),
	(26, 'two dims: scan',    'SELECT count(*) FILTER (WHERE kind = 3), count(*) FILTER (WHERE big = 0) FROM %s'),
	(27, 'one test of two dims: scan', 'SELECT count(*) FILTER (WHERE kind = 3 AND big = 0) FROM %s'),
	(28, 'other key: scan',   'SELECT grp, count(*) FILTER (WHERE kind = 3) FROM %s GROUP BY grp'),
	(29, 'filter on a measure: scan', 'SELECT sum(val) FILTER (WHERE val > 0) FROM %s'),
	(30, 'measure not stored: scan', 'SELECT sum(small) FILTER (WHERE kind = 3) FROM %s'),
	(31, 'min, max: scan',    'SELECT min(val) FILTER (WHERE kind = 3), max(val) FILTER (WHERE kind = 3) FROM %s'),
	(32, 'distinct: scan',    'SELECT count(DISTINCT val) FILTER (WHERE kind = 3) FROM %s'),
	(33, 'where on another dim: scan', 'SELECT count(*) FILTER (WHERE kind = 3) FROM %s WHERE big = 0');

-- one row per query: whether the three ways agree, and how much the first answered from metadata
CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 33) RETURNS TABLE (n int, shape text, pre text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.pre IS NULL THEN '-' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END,
	       pa_rows(z.q, true, true) IS NOT DISTINCT FROM pa_rows(z.q, false, true) AND
	       pa_rows(z.q, false, true) IS NOT DISTINCT FROM pa_rows(z.q, false, false)
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pa_q WHERE n BETWEEN lo AND hi) z, LATERAL pa_units(z.q) u ORDER BY z.n
$$;

-- the same, summed up for a table whose answers need not be listed
CREATE FUNCTION pa_sum(tbl text, lo int DEFAULT 1, hi int DEFAULT 33) RETURNS TABLE (queries bigint, differ bigint, all_pre bigint, some_pre bigint, no_pre bigint) LANGUAGE plpgsql AS $$
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
SELECT n, replace(pa_rows(format(q, 'ec'), true, true), E'\n', ' / ') AS answer FROM pa_q WHERE n NOT IN (17, 28) ORDER BY n;

-- the plan counts the units answered from the pre-aggregates
SELECT units, pre FROM pa_units('SELECT count(*) FILTER (WHERE kind IN (1, 4, 6)), sum(val) FILTER (WHERE kind IN (1, 4, 6)) FROM ec');

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
SELECT count(*) FILTER (WHERE kind = 8), sum(val) FILTER (WHERE kind = 8) FROM ec;
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
SELECT * FROM pa_check('ec');
UPDATE ec SET small = small WHERE id < 20000;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('ec');
SELECT n, (u).workers > 0 AS workers, (u).pre > 0 AS some_pre
FROM (SELECT n, pa_units(format(q, 'ec')) AS u FROM pa_q WHERE n IN (1, 8, 15)) z ORDER BY n;
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
SELECT pre IS NULL AS off FROM pa_units('SELECT count(*) FILTER (WHERE kind = 3) FROM ed');
RESET sift.agg_preagg;
SELECT pre > 0 AS on FROM pa_units('SELECT count(*) FILTER (WHERE kind = 3) FROM ed');

DROP TABLE ec, ed, eo, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int);
DROP PROCEDURE pa_settle();
DROP EXTENSION pg_sift;
