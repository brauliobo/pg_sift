-- SiftAgg answers the units that are exact from the pre-aggregates the sift_col index stored with them (docs/COL-PREAGG.md section 6).
-- Every query runs three ways, which must give the same rows: from the pre-aggregates, scanning every unit (sift.agg_preagg off), and
-- the plain plan (sift.enable_agg off). `pre` says how many units the first one answered from metadata: all, some, none ('-': no SiftAgg).
CREATE EXTENSION pg_sift;
SET default_statistics_target = 10000;
SET sift.agg_dead_delta = 0;	-- a unit with a dead row is scanned here (col_dead_delta answers it net of the dead rows)
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
-- workers divide the heap blocks among themselves as they run, so a parallel build lays the rows out in other units from run to run, and the counts of
-- units answered from the pre-aggregates follow the layout: every build is serial except the one of step 6
SET max_parallel_maintenance_workers = 0;

CREATE TABLE ap (id int, grp text, cat int, d date, amt numeric(10,2), qty int, big bigint, huge numeric(18, 0), f float8, ts timestamp, pad text) WITH (autovacuum_enabled = off);

\ir test/agg_preagg_helpers.sql

\ir test/agg_preagg_units.sql

CREATE TABLE pa_q (n int, shape text, q text);
INSERT INTO pa_q VALUES
	(1, 'key text',          'SELECT grp, count(*) FROM %s GROUP BY grp'),
	(2, 'key int',           'SELECT cat, count(*), sum(amt) FROM %s GROUP BY cat'),
	(3, 'key date',          'SELECT d, sum(amt) FROM %s GROUP BY d'),
	(4, 'key int, avg',      'SELECT qty, count(*), sum(amt), avg(amt) FROM %s GROUP BY qty'),
	(5, 'measures',          'SELECT grp, sum(amt), sum(qty), count(amt), avg(qty) FROM %s GROUP BY grp'),
	(6, 'sum past int64',    'SELECT grp, sum(huge), count(huge), avg(huge) FROM %s GROUP BY grp'),
	(7, 'whole table',       'SELECT count(*), sum(amt), min(qty), max(qty), avg(qty), count(grp), count(amt), min(d), max(ts), min(big), max(big), sum(huge) FROM %s'),
	(8, 'zone covers all',   'SELECT count(*), sum(amt) FROM %s WHERE id >= 1'),
	(9, 'zone covers some',  'SELECT count(*), sum(amt) FROM %s WHERE id < 20000'),
	(10, 'key in list',      'SELECT grp, count(*) FROM %s WHERE grp IN (''g01'', ''g02'', ''g03'') GROUP BY grp'),
	(11, 'test of dim',      'SELECT count(*), sum(amt) FROM %s WHERE grp = ''g05'''),
	(12, 'null group',       'SELECT count(*), sum(amt) FROM %s WHERE grp IS NULL'),
	(13, 'text expr',        'SELECT grp, count(*) FROM %s WHERE grp LIKE ''g1%%'' GROUP BY grp'),
	(14, 'int compare',      'SELECT cat, sum(amt) FROM %s WHERE cat > 3 GROUP BY cat'),
	(15, 'date range',       'SELECT d, count(*) FROM %s WHERE d >= ''2024-06-01'' AND d < ''2024-09-01'' GROUP BY d'),
	(16, 'zone and key',     'SELECT grp, sum(amt) FROM %s WHERE ts >= ''2024-01-05'' GROUP BY grp'),
	(17, 'float: scan',      'SELECT grp, sum(f), count(*) FROM %s GROUP BY grp'),
	(18, 'other test: scan', 'SELECT grp, sum(amt) FROM %s WHERE qty = 3 GROUP BY grp'),
	(19, 'having, limit',    'SELECT grp, count(*) FROM %s GROUP BY grp HAVING count(*) > 100 ORDER BY 2 DESC, 1 LIMIT 5'),
	(20, 'no rows',          'SELECT count(*), sum(amt), min(qty) FROM %s WHERE id > 100000000'),
	(21, 'count of a measure', 'SELECT cat, count(cat), count(amt) FROM %s GROUP BY cat'),
	(22, 'nan: count',       'SELECT grp, count(*) FROM %s GROUP BY grp'),
	(23, 'nan: sum',         'SELECT grp, sum(amt) FROM %s GROUP BY grp'),
	(24, 'nan: other sum',   'SELECT grp, sum(qty) FROM %s GROUP BY grp'),
	(25, 'nan: whole',       'SELECT count(*), sum(qty), min(qty), count(amt) FROM %s'),
	(26, 'nan: whole sum',   'SELECT sum(amt) FROM %s');

-- one row per query: whether the three ways agree (the scan may refuse a column it cannot read, as it does without pre-aggregates), and how much the first answered from metadata
CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 21) RETURNS TABLE (n int, shape text, pre text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.pre IS NULL THEN '-' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END,
	       pa_rows(z.q, true, true) IS NOT DISTINCT FROM pa_rows(z.q, false, true) AND
	       (pa_rows(z.q, false, true) IS NOT DISTINCT FROM pa_rows(z.q, false, false) OR pa_rows(z.q, false, true) LIKE 'ERROR: sift: a column of the aggregate scan holds%')
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pa_q WHERE n BETWEEN lo AND hi) z, LATERAL pa_units(z.q) u ORDER BY z.n
$$;

-- the same, summed up for a table whose answers need not be listed
CREATE FUNCTION pa_sum(tbl text, lo int DEFAULT 1, hi int DEFAULT 21) RETURNS TABLE (queries bigint, differ bigint, all_pre bigint, some_pre bigint, no_pre bigint) LANGUAGE plpgsql AS $$
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

INSERT INTO ap
SELECT g, CASE WHEN g % 17 = 0 THEN NULL ELSE 'g' || lpad((g % 30)::text, 2, '0') END, CASE WHEN g % 23 = 0 THEN NULL ELSE g % 10 END,
       date '2024-01-01' + g / 140, CASE WHEN g % 31 = 0 THEN NULL ELSE ((g::bigint * 37) % 100000)::numeric / 100 END, g % 7,
       4611686018427387904 - g % 1000, 999999999999999999 - g, g / 8.0, timestamp '2024-01-01' + g * interval '1 minute', repeat('p', 200)
FROM generate_series(1, 40000) g;

-- the default cube; an explicit cube with several measures; no cube
CREATE TABLE ac (LIKE ap) WITH (autovacuum_enabled = off);
CREATE TABLE ao (LIKE ap) WITH (autovacuum_enabled = off);
INSERT INTO ac SELECT * FROM ap;
INSERT INTO ao SELECT * FROM ap;
CREATE INDEX ap_col ON ap USING sift_col (id, grp, cat, d, amt, qty, big, huge, f, ts) WITH (write_buffer_limit = 4096);
-- the segments of the workers, which the build merges when nothing says otherwise (col_build_standby_space): the layout a build of 3.6.0 left, which an older index still has
SET sift.col_auto_merge_fragmentation = 1000;
CREATE INDEX ac_col ON ac USING sift_col (id, grp, cat, d, amt, qty, big, huge, f, ts)
	WITH (write_buffer_limit = 4096, preagg = 'cube(grp: amt, qty, huge; cat: amt; d: amt; qty: amt)');
RESET sift.col_auto_merge_fragmentation;
CREATE INDEX ao_col ON ao USING sift_col (id, grp, cat, d, amt, qty, big, huge, f, ts) WITH (write_buffer_limit = 4096, preagg = 'off');
CALL pa_settle();
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ap, ac, ao;	-- waits for the cleanup lock: a page another session has pinned (the checkpointer) would stay not all-visible
SELECT sift.col_verify('ac_col', true);
SELECT units > 2 AS units, preagg_units = units AS all_with_cube FROM sift.col_index_info('ac_col');

-- The pad column (not in the indexes) makes a row 300 bytes: a unit is a superblock of 256 pages, about 6000 rows, so 40000 rows are 7 units
-- (200000 narrow rows were 10): enough for units that stay exact next to the ones an update or a delete touches

-- 1. built, every page all-visible
SELECT * FROM pa_check('ac');
SELECT * FROM pa_sum('ap');
SELECT * FROM pa_sum('ao');

-- 2. a few pages change (update, delete): their units are scanned, the others answer
UPDATE ac SET amt = amt + 1, grp = 'g99' WHERE id BETWEEN 100 AND 200;
DELETE FROM ac WHERE id BETWEEN 18000 AND 18100;
UPDATE ap SET amt = amt + 1, grp = 'g99' WHERE id BETWEEN 100 AND 200;
DELETE FROM ap WHERE id BETWEEN 18000 AND 18100;
UPDATE ao SET amt = amt + 1 WHERE id BETWEEN 100 AND 200;
SELECT * FROM pa_check('ac');
SELECT * FROM pa_sum('ap');
SELECT * FROM pa_sum('ao');

-- 3. vacuumed (the index is vacuumed too: with few dead rows VACUUM would skip it): the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) ac, ap, ao;
SELECT * FROM pa_check('ac');

-- 4. rows in the write buffer, then sealed
INSERT INTO ac SELECT g, CASE WHEN g % 5 = 0 THEN NULL ELSE 'g' || (g % 40) END, g % 12, date '2025-01-01' + g % 50, g % 997, g % 9, g, g, g, timestamp '2025-01-01' + g * interval '1 minute', repeat('p', 200)
FROM generate_series(300001, 301500) g;
SELECT * FROM pa_check('ac');
SELECT sift.col_promote('ac_col') >= 0 AS promoted;
VACUUM (freeze, index_cleanup on, disable_page_skipping) ac;
SELECT * FROM pa_check('ac');

-- 5. merged: the dead rows are gone and every unit is stored with its pre-aggregates again
SELECT sift.col_merge('ac_col', true) >= 0 AS merged;
VACUUM (freeze, index_cleanup on, disable_page_skipping) ac;
SELECT sift.col_verify('ac_col', true);
SELECT * FROM pa_check('ac');

-- 6. built by parallel workers (several segments)
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_leader_participation = off;
SET sift.col_auto_merge_fragmentation = 1000;
REINDEX INDEX ac_col;
RESET sift.col_auto_merge_fragmentation;
RESET parallel_leader_participation;
SELECT segments > 1 AS parallel_segments FROM sift.col_index_info('ac_col');
-- the layout of a parallel build is not the same twice: its answers must be, the counts are for a serial rebuild
SELECT count(*) FILTER (WHERE NOT same) AS differ FROM pa_check('ac');
SET max_parallel_maintenance_workers = 0;
REINDEX INDEX ac_col;
SELECT * FROM pa_check('ac');

-- half of the units change and the rest answer, through the workers of the scan
UPDATE ac SET qty = qty WHERE id < 20000;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('ac');
SELECT n, (u).workers > 0 AS workers, (u).pre > 0 AS some_pre
FROM (SELECT n, pa_units(format(q, 'ac')) AS u FROM pa_q WHERE n IN (1, 7)) z ORDER BY n;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;

-- a transaction that deleted rows sees them gone, another does not until it commits
BEGIN;
DELETE FROM ao WHERE id BETWEEN 1 AND 100;
SELECT * FROM pa_sum('ao');
ROLLBACK;
SELECT * FROM pa_sum('ao');

-- values the index cannot store (a numeric NaN): the units that hold them are scanned, as they are without pre-aggregates
CREATE TABLE an (id int, grp text, amt numeric(10, 2), qty int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO an SELECT g, 'g' || g % 5, CASE WHEN g % 1000 = 0 THEN 'NaN'::numeric ELSE g % 100 END, g % 3, repeat('p', 200) FROM generate_series(1, 30000) g;
CREATE INDEX an_col ON an USING sift_col (id, grp, amt, qty) WITH (preagg = 'cube(grp: amt, qty; qty: amt)');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) an;
SELECT * FROM pa_check('an', 22, 26);

-- Mixed NULL counts of a high-cardinality leaf use its zone counts without a histogram.
CREATE TABLE az (id int, val int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO az SELECT g, CASE WHEN g % 3 = 0 THEN NULL ELSE g END, repeat('p', 200) FROM generate_series(1, 12000) g;
CREATE INDEX az_col ON az USING sift_col (id, val);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) az;
INSERT INTO pa_q VALUES
	(27, 'zone null count', 'SELECT count(*) FROM %s WHERE val IS NULL'),
	(28, 'zone nonnull count', 'SELECT count(*) FROM %s WHERE val IS NOT NULL'),
	(29, 'null count of leaf', 'SELECT count(val) FROM %s WHERE val IS NULL'),
	(30, 'null and range', 'SELECT count(*) FROM %s WHERE val IS NULL AND id < 2000');
SELECT * FROM pa_check('az', 27, 30);
BEGIN;
UPDATE az SET val = NULL WHERE id BETWEEN 20 AND 50;
DELETE FROM az WHERE id % 97 = 0;
SELECT count(*) FILTER (WHERE NOT same) AS differ FROM pa_check('az', 27, 30);
ROLLBACK;
DELETE FROM az WHERE id % 97 = 0;
VACUUM (FREEZE, INDEX_CLEANUP ON, DISABLE_PAGE_SKIPPING) az;
SELECT count(*) FILTER (WHERE NOT same) AS differ FROM pa_check('az', 27, 30);
SELECT sift.col_merge('az_col', true) >= 0 AS merged;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) az;
SELECT * FROM pa_check('az', 27, 30);
DROP TABLE az;

-- the switches
SET sift.agg_preagg = off;
SELECT pre IS NULL AS off FROM pa_units('SELECT grp, count(*) FROM ac GROUP BY grp');
RESET sift.agg_preagg;
SELECT pre > 0 AS on FROM pa_units('SELECT grp, count(*) FROM ac GROUP BY grp');

DROP TABLE ap, ac, ao, an, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int);
DROP PROCEDURE pa_settle();
DROP EXTENSION pg_sift;
