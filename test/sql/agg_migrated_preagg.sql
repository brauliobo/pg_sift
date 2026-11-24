-- The pre-aggregates of the column index (cubes, filters, measures, integer keys) held to the normal plan: every query gives the rows of the plain plan whichever units answer it from metadata.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
\set VERBOSITY terse

-- the rows of q as sorted jsonb text, or its error, with the aggregate scan on or off
CREATE FUNCTION mig_run(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), ''[]'')::text FROM (%s) t', q) INTO r;
	RETURN r;
EXCEPTION WHEN others THEN RETURN 'ERR ' || SQLSTATE || ' ' || SQLERRM;
END $$;
-- 'same' when the rows (or the error) of q are those of the normal plan, else both cut to 200 characters
CREATE FUNCTION mig_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := mig_run(q, false); b text := mig_run(q, true);
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;
-- the control: a statement whose rows depend on the setting is told apart
SELECT mig_same($$SELECT current_setting('sift.enable_agg') AS v$$) LIKE 'DIFF%' AS control_detects_a_difference;

-- A snapshot held by another session of this database (the refresh worker rewriting a segment, one that is still ending) keeps the pages of rows written
-- since from becoming all-visible, so the units stay scanned: pa_settle() waits for the queue drained and no such session.
\set ECHO none
CREATE PROCEDURE pa_settle() LANGUAGE plpgsql AS $$
DECLARE deadline timestamptz := clock_timestamp() + interval '120 seconds';
BEGIN
	WHILE EXISTS (SELECT 1 FROM sift.col_rollup_queue())
	   OR EXISTS (SELECT 1 FROM pg_stat_activity WHERE datname = current_database() AND pid <> pg_backend_pid() AND backend_xmin IS NOT NULL) LOOP
		IF clock_timestamp() > deadline THEN
			RAISE EXCEPTION 'not settled: queue %, snapshots %', (SELECT count(*) FROM sift.col_rollup_queue()),
				(SELECT string_agg(backend_type || ' ' || backend_xmin, ', ') FROM pg_stat_activity
				 WHERE datname = current_database() AND pid <> pg_backend_pid() AND backend_xmin IS NOT NULL);
		END IF;
		PERFORM pg_sleep(0.01);
	END LOOP;
END $$;
\set ECHO all
-- the sorted rows of q, or its error, with the aggregate scan on (pa_rows) or the plain plan, which no worker helps (pa_plain)
CREATE FUNCTION pa_exec(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;
CREATE FUNCTION pa_rows(q text) RETURNS text LANGUAGE sql SET sift.enable_agg = on AS $$ SELECT pa_exec(q) $$;
CREATE FUNCTION pa_plain(q text) RETURNS text LANGUAGE sql SET sift.enable_agg = off SET max_parallel_workers_per_gather = 0 AS $$ SELECT pa_exec(q) $$;
-- one row per query of pa_q (those marked w only, for a pass that starts workers for every query) in the schema of the segment:
-- whether the aggregate scan and the plain plan agree
CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 1000, w_only bool DEFAULT false) RETURNS TABLE (n int, shape text, same bool) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT z.n, z.shape, pa_rows(z.q) IS NOT DISTINCT FROM pa_plain(z.q)
	FROM (SELECT p.n, p.shape, format(p.q, tbl) AS q FROM pa_q p WHERE p.n BETWEEN lo AND hi AND (p.w OR NOT w_only)) z ORDER BY z.n;
END $$;
-- the same, summed up for a table whose answers need not be listed
CREATE FUNCTION pa_sum(tbl text, lo int DEFAULT 1, hi int DEFAULT 1000, w_only bool DEFAULT false) RETURNS TABLE (queries bigint, differ bigint) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT count(*), count(*) FILTER (WHERE NOT c.same) FROM pa_check(tbl, lo, hi, w_only) c;
END $$;

-- >>> preagg
SET client_min_messages = warning;
CREATE SCHEMA m1;
GRANT USAGE ON SCHEMA m1 TO PUBLIC;
SET search_path = m1, public;
-- SiftAgg answers the units that are exact from the pre-aggregates the sift_col index stored with them (docs/COL-PREAGG.md section 6).
-- Every query runs two ways, which must give the same rows: the aggregate scan, which answers from metadata the units that are exact, and
-- the plain plan (sift.enable_agg off).
SET default_statistics_target = 10000;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
-- workers divide the heap blocks among themselves as they run, so a parallel build lays the rows out in other units from run to run: every build is
-- serial except the one of step 6
SET max_parallel_maintenance_workers = 0;
-- a unit is a superblock of 256 pages: a pad that fills a page with one row makes a unit of 256 rows, so 832 rows are 3 units and a tail of 64 rows
CREATE TABLE ap (id int, grp text, cat int, d date, amt numeric(10,2), qty int, big bigint, huge numeric(18, 0), f float8, ts timestamp, pad text STORAGE PLAIN) WITH (autovacuum_enabled = off);
CREATE TABLE pa_q (n int, shape text, q text, w bool DEFAULT false);
INSERT INTO pa_q VALUES
	(1, 'key text',          'SELECT grp, count(*) FROM %s GROUP BY grp'),
	(2, 'key int',           'SELECT cat, count(*), sum(amt) FROM %s GROUP BY cat'),
	(3, 'key date',          'SELECT d, sum(amt) FROM %s GROUP BY d'),
	(4, 'key int, avg',      'SELECT qty, count(*), sum(amt), avg(amt) FROM %s GROUP BY qty'),
	(5, 'measures',          'SELECT grp, sum(amt), sum(qty), count(amt), avg(qty) FROM %s GROUP BY grp'),
	(6, 'sum past int64',    'SELECT grp, sum(huge), count(huge), avg(huge) FROM %s GROUP BY grp'),
	(7, 'whole table',       'SELECT count(*), sum(amt), min(qty), max(qty), avg(qty), count(grp), count(amt), min(d), max(ts), min(big), max(big), sum(huge) FROM %s'),
	(8, 'zone covers all',   'SELECT count(*), sum(amt) FROM %s WHERE id >= 1'),
	(9, 'zone covers some',  'SELECT count(*), sum(amt) FROM %s WHERE id < 600'),
	(10, 'key in list',      'SELECT grp, count(*) FROM %s WHERE grp IN (''g01'', ''g02'', ''g03'') GROUP BY grp'),
	(11, 'test of dim',      'SELECT count(*), sum(amt) FROM %s WHERE grp = ''g05'''),
	(12, 'null group',       'SELECT count(*), sum(amt) FROM %s WHERE grp IS NULL'),
	(13, 'text expr',        'SELECT grp, count(*) FROM %s WHERE grp LIKE ''g1%%'' GROUP BY grp'),
	(14, 'int compare',      'SELECT cat, sum(amt) FROM %s WHERE cat > 3 GROUP BY cat'),
	(15, 'date range',       'SELECT d, count(*) FROM %s WHERE d >= ''2024-06-01'' AND d < ''2024-09-01'' GROUP BY d'),
	(16, 'zone and key',     'SELECT grp, sum(amt) FROM %s WHERE ts >= ''2024-01-05'' GROUP BY grp'),
	(17, 'float: scan',      'SELECT grp, sum(f), count(*) FROM %s GROUP BY grp'),
	(18, 'other test: scan', 'SELECT grp, sum(amt) FROM %s WHERE qty = 3 GROUP BY grp'),
	(19, 'having, limit',    'SELECT grp, count(*) FROM %s GROUP BY grp HAVING count(*) > 50 ORDER BY 2 DESC, 1 LIMIT 5'),
	(20, 'no rows',          'SELECT count(*), sum(amt), min(qty) FROM %s WHERE id > 100000000'),
	(21, 'count of a measure', 'SELECT cat, count(cat), count(amt) FROM %s GROUP BY cat'),
	(22, 'nan: count',       'SELECT grp, count(*) FROM %s GROUP BY grp'),
	(23, 'nan: sum',         'SELECT grp, sum(amt) FROM %s GROUP BY grp'),
	(24, 'nan: other sum',   'SELECT grp, sum(qty) FROM %s GROUP BY grp'),
	(25, 'nan: whole',       'SELECT count(*), sum(qty), min(qty), count(amt) FROM %s'),
	(26, 'nan: whole sum',   'SELECT sum(amt) FROM %s');
-- w: the queries of the pass through the workers of the scan, one or more of each shape of aggregate state (every query of a pass starts workers)
UPDATE pa_q SET w = true WHERE n IN (1, 2, 3, 6, 7, 9, 12, 17, 19, 20);
INSERT INTO ap
SELECT g, CASE WHEN g % 17 = 0 THEN NULL ELSE 'g' || lpad((g % 30)::text, 2, '0') END, CASE WHEN g % 23 = 0 THEN NULL ELSE g % 10 END,
       date '2024-01-01' + g / 4, CASE WHEN g % 31 = 0 THEN NULL ELSE ((g::bigint * 37) % 100000)::numeric / 100 END, g % 7,
       4611686018427387904 - g % 1000, 999999999999999999 - g, g / 8.0, timestamp '2024-01-01' + g * interval '25 minutes', repeat('p', 4100)
FROM generate_series(1, 832) g;
-- the default cube; an explicit cube with several measures; no cube
CREATE TABLE ac (LIKE ap INCLUDING STORAGE) WITH (autovacuum_enabled = off);
CREATE TABLE ao (LIKE ap INCLUDING STORAGE) WITH (autovacuum_enabled = off);
INSERT INTO ac SELECT * FROM ap;
INSERT INTO ao SELECT * FROM ap;
CREATE INDEX ap_col ON ap USING sift_col (id, grp, cat, d, amt, qty, big, huge, f, ts) WITH (write_buffer_limit = 4096);
CREATE INDEX ac_col ON ac USING sift_col (id, grp, cat, d, amt, qty, big, huge, f, ts)
	WITH (write_buffer_limit = 4096, preagg = 'cube(grp: amt, qty, huge; cat: amt; d: amt; qty: amt)');
CREATE INDEX ao_col ON ao USING sift_col (id, grp, cat, d, amt, qty, big, huge, f, ts) WITH (write_buffer_limit = 4096, preagg = 'off');
CALL pa_settle();
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ap, ac, ao;
SELECT sift.col_verify('ac_col', true);
-- 1. built, every page all-visible
SELECT * FROM pa_check('ac');
SELECT * FROM pa_sum('ap');
SELECT * FROM pa_sum('ao');
-- 2. a few pages change (update, delete): their units are scanned, the others answer
UPDATE ac SET amt = amt + 1, grp = 'g99' WHERE id BETWEEN 10 AND 20;
DELETE FROM ac WHERE id BETWEEN 530 AND 540;
UPDATE ap SET amt = amt + 1, grp = 'g99' WHERE id BETWEEN 10 AND 20;
DELETE FROM ap WHERE id BETWEEN 530 AND 540;
UPDATE ao SET amt = amt + 1 WHERE id BETWEEN 10 AND 20;
SELECT * FROM pa_check('ac');
SELECT * FROM pa_sum('ap');
SELECT * FROM pa_sum('ao');
-- 3. vacuumed (the index is vacuumed too: with few dead rows VACUUM would skip it): the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) ac, ap, ao;
SELECT * FROM pa_check('ac');
-- 4. rows in the write buffer, then sealed
INSERT INTO ac SELECT g, CASE WHEN g % 5 = 0 THEN NULL ELSE 'g' || (g % 40) END, g % 12, date '2025-01-01' + g % 50, g % 997, g % 9, g, g, g, timestamp '2025-01-01' + g * interval '1 minute', repeat('p', 4100)
FROM generate_series(300001, 300300) g;
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
REINDEX INDEX ac_col;
RESET parallel_leader_participation;
-- the layout of a parallel build is not the same twice: its answers must be, the counts are for a serial rebuild
SELECT count(*) FILTER (WHERE NOT same) AS differ FROM pa_check('ac');
SET max_parallel_maintenance_workers = 0;
REINDEX INDEX ac_col;
SELECT * FROM pa_check('ac');
-- half of the units change and the rest answer, through the workers of the scan
UPDATE ac SET qty = qty WHERE id < 600;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('ac', w_only => true);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
-- a transaction that deleted rows sees them gone, another does not until it commits
BEGIN;
DELETE FROM ao WHERE id BETWEEN 1 AND 20;
SELECT * FROM pa_sum('ao');
ROLLBACK;
SELECT * FROM pa_sum('ao');
-- values the index cannot store (a numeric NaN): the units that hold them are scanned, as they are without pre-aggregates
CREATE TABLE an (id int, grp text, amt numeric(10, 2), qty int, pad text STORAGE PLAIN) WITH (autovacuum_enabled = off);
INSERT INTO an SELECT g, 'g' || g % 5, CASE WHEN g % 300 = 0 THEN 'NaN'::numeric ELSE g % 100 END, g % 3, repeat('p', 4100) FROM generate_series(1, 832) g;
CREATE INDEX an_col ON an USING sift_col (id, grp, amt, qty) WITH (preagg = 'cube(grp: amt, qty; qty: amt)');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) an;
SELECT * FROM pa_check('an', 22, 26);
-- Mixed NULL counts of a high-cardinality leaf use its zone counts without a histogram: two rows to a page, so a unit of 512 rows has 341 values, more than
-- the 256 groups a histogram holds.
CREATE TABLE az (id int, val int, pad text STORAGE PLAIN) WITH (autovacuum_enabled = off);
INSERT INTO az SELECT g, CASE WHEN g % 3 = 0 THEN NULL ELSE g END, repeat('p', 3000) FROM generate_series(1, 812) g;
CREATE INDEX az_col ON az USING sift_col (id, val);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) az;
INSERT INTO pa_q VALUES
	(27, 'zone null count', 'SELECT count(*) FROM %s WHERE val IS NULL'),
	(28, 'zone nonnull count', 'SELECT count(*) FROM %s WHERE val IS NOT NULL'),
	(29, 'null count of leaf', 'SELECT count(val) FROM %s WHERE val IS NULL'),
	(30, 'null and range', 'SELECT count(*) FROM %s WHERE val IS NULL AND id < 300');
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
DROP TABLE ap, ac, ao, an, az, pa_q;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m1 CASCADE;
-- <<< preagg

-- >>> dimfilter, filter
SET client_min_messages = warning;
CREATE SCHEMA m2;
GRANT USAGE ON SCHEMA m2 TO PUBLIC;
SET search_path = m2, public;
-- A filter on a dimension of the cube (`kind = 3`, `kind IN (1, 4)`, a range) with no group key is answered from the histogram of that dimension: the
-- groups that pass the filter are added up for the units whose zone is not inside it, the sums of the leaves for those whose zone is.
-- A FILTER on the dimension (`count(*) FILTER (WHERE kind = 3)`) is answered from the same histogram: the groups that pass the FILTER add to the
-- aggregate, the others add nothing; the queries 101 and up are the ones with FILTER.
-- Every query runs two ways, which must give the same rows (text, so the scale of the numeric counts): the aggregate scan, which answers from
-- metadata the units that are exact, and the plain plan (sift.enable_agg off).
SET default_statistics_target = 10000;
-- a unit is a superblock of 256 pages: a pad that fills a page with one row makes a unit of 256 rows, so 832 rows are 3 units and a tail of 64 rows
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
-- workers divide the heap blocks among themselves as they run, so a parallel build lays the rows out in other units from run to run, and the counts of
-- units answered from the pre-aggregates follow the layout: every build is serial except the one of step 6
SET max_parallel_maintenance_workers = 0;
CREATE TABLE ec (id int, kind int, big bigint, grp text, val int, small int, amt numeric(12, 2), pad text STORAGE PLAIN) WITH (autovacuum_enabled = off);
CREATE TABLE pa_q (n int, shape text, q text, w bool DEFAULT false);
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
	(16, 'eq, zone on the id', 'SELECT count(*), sum(val) FROM %s WHERE kind = 3 AND id < 600'),
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
	(32, 'the dim and its text', 'SELECT count(*), sum(val) FROM %s WHERE kind = 3 AND grp = ''g05'''),
	(101, 'count',              'SELECT count(*) FILTER (WHERE kind = 3) FROM %s'),
	(102, 'count and total',    'SELECT count(*), count(*) FILTER (WHERE kind = 3) FROM %s'),
	(103, 'sums',               'SELECT sum(val) FILTER (WHERE kind = 3), avg(val) FILTER (WHERE kind = 3), count(val) FILTER (WHERE kind = 3), sum(amt) FILTER (WHERE kind = 3), avg(amt) FILTER (WHERE kind = 3), count(amt) FILTER (WHERE kind = 3) FROM %s'),
	(104, 'two filters',        'SELECT count(*) FILTER (WHERE kind = 1), count(*) FILTER (WHERE kind = 2), sum(val) FILTER (WHERE kind IN (3, 4)), sum(val), count(*) FROM %s'),
	(105, 'zero, the NULL group', 'SELECT count(*) FILTER (WHERE kind = 0), sum(val) FILTER (WHERE kind = 0) FROM %s'),
	(106, 'absent',             'SELECT count(*) FILTER (WHERE kind = 99), sum(val) FILTER (WHERE kind = 99), avg(amt) FILTER (WHERE kind = 99) FROM %s'),
	(107, 'down',           'SELECT count(*) FILTER (WHERE kind = -2), sum(val) FILTER (WHERE kind = -2) FROM %s'),
	(108, 'in list, gaps',      'SELECT count(*) FILTER (WHERE kind IN (1, 4, 6)), sum(val) FILTER (WHERE kind IN (1, 4, 6)) FROM %s'),
	(109, 'in list, NULL',      'SELECT count(*) FILTER (WHERE kind IN (2, NULL)), sum(val) FILTER (WHERE kind IN (2, NULL)) FROM %s'),
	(110, 'range',             'SELECT count(*) FILTER (WHERE kind >= 4), sum(val) FILTER (WHERE kind > 1 AND kind < 5) FROM %s'),
	(111, 'not equal',         'SELECT count(*) FILTER (WHERE kind <> 3), sum(val) FILTER (WHERE kind <> 3) FROM %s'),
	(112, 'IS NULL',           'SELECT count(*) FILTER (WHERE kind IS NULL), sum(val) FILTER (WHERE kind IS NULL), count(kind) FILTER (WHERE kind IS NULL) FROM %s'),
	(113, 'OR on the dim',     'SELECT count(*) FILTER (WHERE kind = 1 OR kind = 5), sum(val) FILTER (WHERE kind = 1 OR kind IS NULL) FROM %s'),
	(114, 'count of the dim',  'SELECT count(kind) FILTER (WHERE kind = 3), count(kind) FILTER (WHERE kind > 2), count(kind) FROM %s'),
	(115, 'key is the dim',    'SELECT kind, count(*), count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind > 2), sum(val) FROM %s GROUP BY kind'),
	(116, 'key, a group never passes', 'SELECT kind, count(*) FILTER (WHERE kind = 99), sum(val) FILTER (WHERE kind = 99), avg(amt) FILTER (WHERE kind = 99) FROM %s GROUP BY kind'),
	(117, 'text key',          'SELECT grp, count(*) FILTER (WHERE grp = ''g05''), sum(val) FILTER (WHERE grp IN (''g05'', ''g06'')), count(*) FROM %s GROUP BY grp'),
	(118, 'where on the dim',  'SELECT count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind > 1), sum(val) FROM %s WHERE kind <> 2'),
	(119, 'key and where',     'SELECT kind, count(*) FILTER (WHERE kind IN (1, 3)), sum(val) FILTER (WHERE kind <> 4) FROM %s WHERE kind > 0 GROUP BY kind'),
	(120, 'zone on the id',    'SELECT count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind = 3) FROM %s WHERE id < 600'),
	(121, 'zone covers',       'SELECT count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind = 3) FROM %s WHERE id >= 1'),
	(122, 'no rows',           'SELECT count(*) FILTER (WHERE kind = 3), sum(val) FILTER (WHERE kind = 3) FROM %s WHERE id > 100000000'),
	(123, 'int8 dim',          'SELECT count(*) FILTER (WHERE big = 4611686018427387905), sum(val) FILTER (WHERE big IN (0, 7)), sum(amt) FILTER (WHERE big IS NULL) FROM %s'),
	(124, 'int8 extremes',     'SELECT count(*) FILTER (WHERE big IN (9223372036854775807, -9223372036854775807 - 1)), sum(val) FILTER (WHERE big < 0) FROM %s'),
	(125, 'text dim',          'SELECT count(*) FILTER (WHERE grp = ''g05''), sum(val) FILTER (WHERE grp = ''g05''), count(*) FILTER (WHERE grp IS NULL) FROM %s'),
	(126, 'two dims: scan',    'SELECT count(*) FILTER (WHERE kind = 3), count(*) FILTER (WHERE big = 0) FROM %s'),
	(127, 'one test of two dims: scan', 'SELECT count(*) FILTER (WHERE kind = 3 AND big = 0) FROM %s'),
	(128, 'other key: scan',   'SELECT grp, count(*) FILTER (WHERE kind = 3) FROM %s GROUP BY grp'),
	(129, 'filter on a measure: scan', 'SELECT sum(val) FILTER (WHERE val > 0) FROM %s'),
	(130, 'measure not stored: scan', 'SELECT sum(small) FILTER (WHERE kind = 3) FROM %s'),
	(131, 'min, max: scan',    'SELECT min(val) FILTER (WHERE kind = 3), max(val) FILTER (WHERE kind = 3) FROM %s'),
	(132, 'distinct: scan',    'SELECT count(DISTINCT val) FILTER (WHERE kind = 3) FROM %s'),
	(133, 'where on another dim: scan', 'SELECT count(*) FILTER (WHERE kind = 3) FROM %s WHERE big = 0');
-- w: the queries of the pass through the workers of the scan, one or more of each shape of aggregate state (every query of a pass starts workers)
UPDATE pa_q SET w = true WHERE n IN (2, 3, 4, 6, 11, 14, 16, 19, 25, 28, 29, 31, 103, 104, 106, 112, 115, 117, 120, 123, 128, 132);
-- one row per query: whether the three ways agree, and how much the first answered from metadata
-- the same, summed up for a table whose answers need not be listed
-- kind: 0 to 6 (a ninth of the rows NULL, -2 for one row) in every unit, so no zone lies inside a comparison with it; big: a few int8 values with the
-- extremes and NULLs, the sign and 2^62 as dims; val and amt: NULLs among the values
INSERT INTO ec
SELECT g, CASE WHEN g % 9 = 0 THEN NULL WHEN g = 1000 THEN -2 ELSE g % 7 END,
       CASE g % 6 WHEN 0 THEN 4611686018427387905 WHEN 1 THEN -4611686018427387905 WHEN 2 THEN 0 WHEN 3 THEN NULL
                  WHEN 4 THEN CASE WHEN g % 12 = 4 THEN 9223372036854775807 ELSE -9223372036854775807 - 1 END ELSE 7 END,
       CASE WHEN g % 17 = 0 THEN NULL ELSE 'g' || lpad((g % 30)::text, 2, '0') END,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 37) % 1000 - 400 END, g % 13,
       CASE WHEN g % 19 = 0 THEN NULL ELSE ((g::bigint * 37) % 100000)::numeric / 100 END, repeat('p', 4100)
FROM generate_series(1, 832) g;
-- an explicit cube with the measures; the default cube; no cube
CREATE TABLE ed (LIKE ec INCLUDING STORAGE) WITH (autovacuum_enabled = off);
CREATE TABLE eo (LIKE ec INCLUDING STORAGE) WITH (autovacuum_enabled = off);
INSERT INTO ed SELECT * FROM ec;
INSERT INTO eo SELECT * FROM ec;
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
SELECT n, replace(pa_rows(format(q, 'ec')), E'\n', ' / ') AS answer FROM pa_q WHERE n NOT IN (28, 117, 128) ORDER BY n;
-- the plan counts the units answered from the pre-aggregates
-- 2. a few pages change (update, delete): their units are scanned, the others answer
UPDATE ec SET val = -(val / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 10 AND 20;
DELETE FROM ec WHERE id BETWEEN 530 AND 540;
UPDATE ed SET val = -(val / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 10 AND 20;
DELETE FROM ed WHERE id BETWEEN 530 AND 540;
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');
-- 3. vacuumed: the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec, ed;
SELECT * FROM pa_check('ec');
-- 4. rows in the write buffer (a kind and a big no unit holds), then sealed
INSERT INTO ec SELECT g, CASE WHEN g % 4 = 0 THEN NULL ELSE 7 + g % 3 END, CASE WHEN g % 3 = 0 THEN 4611686018427387905 ELSE 5 END,
                  'g' || (g % 40), g % 9, g % 5, g % 100, repeat('p', 4100)
FROM generate_series(300001, 300300) g;
SELECT * FROM pa_check('ec');
SELECT count(*), sum(val) FROM ec WHERE kind = 8;
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
-- the layout of a parallel build is not the same twice: its answers must be, the counts are for a serial rebuild
SELECT count(*) FILTER (WHERE NOT same) AS differ FROM pa_check('ec');
SET max_parallel_maintenance_workers = 0;
REINDEX INDEX ec_col;
SELECT * FROM pa_check('ec');
UPDATE ec SET small = small WHERE id < 600;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('ec', w_only => true);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
-- a transaction that deleted rows sees them gone, another does not until it commits
BEGIN;
DELETE FROM eo WHERE id BETWEEN 1 AND 20;
SELECT * FROM pa_sum('eo');
ROLLBACK;
SELECT * FROM pa_sum('eo');
-- the switch
DROP TABLE ec, ed, eo, pa_q;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m2 CASCADE;
-- <<< dimfilter, filter

-- >>> int8
SET client_min_messages = warning;
CREATE SCHEMA m4;
GRANT USAGE ON SCHEMA m4 TO PUBLIC;
SET search_path = m4, public;
-- sum(int8) and avg(int8) are numeric aggregates over an exact 128-bit sum: the units that are exact answer them from the stored sums.
-- Every query runs two ways, which must give the same rows (text, so the scale of the numeric counts): the aggregate scan, which answers from
-- metadata the units that are exact, and the plain plan (sift.enable_agg off).
SET default_statistics_target = 10000;
-- a unit is a superblock of 256 pages: a pad that fills a page with one row makes a unit of 256 rows, so 832 rows are 3 units and a tail of 64 rows
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
-- workers divide the heap blocks among themselves as they run, so a parallel build lays the rows out in other units from run to run, and the counts of
-- units answered from the pre-aggregates follow the layout: every build is serial except the one of step 6
SET max_parallel_maintenance_workers = 0;
CREATE TABLE ec (id int, kind int, grp text, val bigint, small bigint, pad text STORAGE PLAIN) WITH (autovacuum_enabled = off);
CREATE TABLE pa_q (n int, shape text, q text, w bool DEFAULT false);
INSERT INTO pa_q VALUES
	(1, 'whole',               'SELECT sum(val), avg(val), count(val), count(*) FROM %s'),
	(2, 'whole, small values', 'SELECT sum(small), avg(small) FROM %s'),
	(3, 'by kind',             'SELECT kind, sum(val), avg(val), sum(small), avg(small) FROM %s GROUP BY kind'),
	(4, 'by text key',         'SELECT grp, sum(val), avg(val) FROM %s GROUP BY grp'),
	(5, 'only NULLs',          'SELECT sum(val), avg(val), count(val), count(*) FROM %s WHERE kind = 3'),
	(6, 'no rows',             'SELECT sum(val), avg(val), count(*) FROM %s WHERE id > 100000000'),
	(7, 'no rows, grouped',    'SELECT kind, sum(val), avg(val) FROM %s WHERE id > 100000000 GROUP BY kind'),
	(8, 'zone covers all',     'SELECT sum(val), avg(val) FROM %s WHERE id >= 1'),
	(9, 'zone covers some',    'SELECT sum(val), avg(val) FROM %s WHERE id < 600'),
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
-- w: the queries of the pass through the workers of the scan, one or more of each shape of aggregate state (every query of a pass starts workers)
UPDATE pa_q SET w = true WHERE n IN (1, 3, 4, 5, 6, 9, 16, 17);
-- one row per query: whether the three ways agree, and how much the first answered from metadata
-- the same, summed up for a table whose answers need not be listed
-- kind 0: near 2^62 (two rows pass 2^63), kind 1: near -2^62, kind 2: the extremes of int8 with both signs, kind 3: only NULLs,
-- kind 4: small values, a fifth of them NULL, kind 5: values that make the average a repeating decimal
INSERT INTO ec
SELECT g, g % 6, CASE WHEN g % 17 = 0 THEN NULL ELSE 'g' || lpad((g % 30)::text, 2, '0') END,
       CASE g % 6 WHEN 0 THEN 4611686018427387904 - g % 1000
                  WHEN 1 THEN -4611686018427387904 + g % 1000
                  WHEN 2 THEN CASE WHEN g % 12 = 2 THEN 9223372036854775807 - g % 7 ELSE -9223372036854775807 - 1 + g % 7 END
                  WHEN 3 THEN NULL
                  WHEN 4 THEN CASE WHEN g % 5 = 0 THEN NULL ELSE g % 100 - 50 END
                  ELSE (g % 997) * 3 + 1 END,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 37) % 1000 - 400 END, repeat('p', 4100)
FROM generate_series(1, 832) g;
-- an explicit cube with the measures; the default cube; no cube
CREATE TABLE ed (LIKE ec INCLUDING STORAGE) WITH (autovacuum_enabled = off);
CREATE TABLE eo (LIKE ec INCLUDING STORAGE) WITH (autovacuum_enabled = off);
INSERT INTO ed SELECT * FROM ec;
INSERT INTO eo SELECT * FROM ec;
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
UPDATE ec SET val = -(val / 2), small = small + 1 WHERE id BETWEEN 10 AND 20;
DELETE FROM ec WHERE id BETWEEN 530 AND 540;
UPDATE ed SET val = -(val / 2), small = small + 1 WHERE id BETWEEN 10 AND 20;
DELETE FROM ed WHERE id BETWEEN 530 AND 540;
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');
-- 3. vacuumed: the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec, ed;
SELECT * FROM pa_check('ec');
-- 4. rows in the write buffer, then sealed
INSERT INTO ec SELECT g, g % 6, 'g' || (g % 40), CASE WHEN g % 6 = 3 THEN NULL ELSE 4611686018427387904 + g END, g % 9, repeat('p', 4100)
FROM generate_series(300001, 300300) g;
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
-- the layout of a parallel build is not the same twice: its answers must be, the counts are for a serial rebuild
SELECT count(*) FILTER (WHERE NOT same) AS differ FROM pa_check('ec');
SET max_parallel_maintenance_workers = 0;
REINDEX INDEX ec_col;
SELECT * FROM pa_check('ec');
UPDATE ec SET small = small WHERE id < 600;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('ec', w_only => true);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
-- a transaction that deleted rows sees them gone, another does not until it commits
BEGIN;
DELETE FROM eo WHERE id BETWEEN 1 AND 20;
SELECT * FROM pa_sum('eo');
ROLLBACK;
SELECT * FROM pa_sum('eo');
-- the switch
DROP TABLE ec, ed, eo, pa_q;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m4 CASCADE;
-- <<< int8

-- >>> measures
SET client_min_messages = warning;
CREATE SCHEMA m5;
GRANT USAGE ON SCHEMA m5 TO PUBLIC;
SET search_path = m5, public;
-- The default cube keeps up to eight measures of a dim (the first three decide which dims fit the unit's byte budget, the others come with a dim that is kept),
-- so the sum, avg and count of the integer columns beyond the first three answer from the histogram of the group key too.
-- Every query runs two ways, which must give the same rows (text, so the scale of the numeric counts): the aggregate scan, which answers from
-- metadata the units that are exact, and the plain plan (sift.enable_agg off).
SET default_statistics_target = 10000;
-- a unit is a superblock of 256 pages: a pad that fills a page with one row makes a unit of 256 rows, so 832 rows are 3 units and a tail of 64 rows
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
CREATE TABLE ec (id int, kind int, grp text, flag int, amt numeric(12, 2), score float8, n2 int2, n4 int4, n8 bigint, cnt int4, extra int4, pad text STORAGE PLAIN) WITH (autovacuum_enabled = off);
CREATE TABLE pa_q (n int, shape text, q text, w bool DEFAULT false);
INSERT INTO pa_q VALUES
	(1, 'sum int2',           'SELECT kind, sum(n2) FROM %s GROUP BY kind'),
	(2, 'sum, avg int4',      'SELECT kind, sum(n4), avg(n4), count(n4) FROM %s GROUP BY kind'),
	(3, 'sum, avg int8',      'SELECT kind, sum(n8), avg(n8), count(n8) FROM %s GROUP BY kind'),
	(4, 'text key',           'SELECT grp, sum(n8), sum(n4), count(*) FROM %s GROUP BY grp'),
	(5, 'small key',          'SELECT flag, sum(cnt), avg(cnt), sum(n2) FROM %s GROUP BY flag'),
	(6, 'all measures',       'SELECT kind, sum(amt), sum(n2), sum(n4), sum(n8), sum(cnt), count(*) FROM %s GROUP BY kind'),
	(7, 'where on the key',   'SELECT kind, sum(n8), sum(n4) FROM %s WHERE kind IN (1, 3, 5) GROUP BY kind'),
	(8, 'no key',             'SELECT sum(n2), sum(n4), sum(n8), avg(n8) FROM %s'),
	(9, 'no key, where on the dim', 'SELECT sum(n8), sum(n4) FROM %s WHERE kind = 3'),
	(10, 'NULL group',        'SELECT sum(n4), count(*) FROM %s WHERE kind IS NULL'),
	(11, 'absent key',        'SELECT kind, sum(n8) FROM %s WHERE kind = 99 GROUP BY kind'),
	(12, 'a group of NULL measures', 'SELECT flag, sum(cnt), avg(cnt), count(cnt) FROM %s WHERE kind = 6 GROUP BY flag'),
	(13, 'int8 extremes',       'SELECT kind, sum(n8), avg(n8) FROM %s WHERE kind < 3 GROUP BY kind'),
	(14, 'zone on the id',    'SELECT kind, sum(n4) FROM %s WHERE id < 600 GROUP BY kind'),
	(15, 'having, limit',     'SELECT kind, sum(n8) FROM %s GROUP BY kind HAVING sum(n8) > 0 ORDER BY 2 DESC LIMIT 3'),
	(16, 'a measure past the eighth: scan', 'SELECT kind, sum(extra) FROM %s GROUP BY kind'),
	(17, 'min, max: scan',    'SELECT kind, min(n4), max(n8) FROM %s GROUP BY kind'),
	(18, 'expression: scan',  'SELECT kind, sum(n4 + n2) FROM %s GROUP BY kind');
-- w: the queries of the pass through the workers of the scan, one or more of each shape of aggregate state (every query of a pass starts workers)
UPDATE pa_q SET w = true WHERE n IN (2, 4, 6, 8, 9, 10, 13, 16, 18);
-- one row per query: whether the three ways agree, and how much the first answered from metadata
-- the same, summed up for a table whose answers need not be listed
-- kind: 0 to 6 (a ninth of the rows NULL) in every unit; flag: 0 or 1; n2, n4, n8, cnt: NULLs among the values, n8 up to the int8 extremes
INSERT INTO ec
SELECT g, CASE WHEN g % 9 = 0 THEN NULL ELSE g % 7 END,
       CASE WHEN g % 17 = 0 THEN NULL ELSE 'g' || lpad((g % 30)::text, 2, '0') END,
       g % 2, CASE WHEN g % 19 = 0 THEN NULL ELSE ((g::bigint * 37) % 100000)::numeric / 100 END,
       (g % 1000) / 7.0,
       CASE WHEN g % 13 = 0 THEN NULL ELSE (g % 200 - 100)::int2 END,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 37) % 100000 - 40000 END,
       CASE WHEN g % 23 = 0 THEN NULL WHEN g % 1000 = 7 THEN 9223372036854775807 WHEN g % 1000 = 8 THEN -9223372036854775807 - 1 ELSE (g::bigint * 977) % 1000000 END,
       CASE WHEN g % 9 <> 0 AND g % 7 = 6 THEN NULL ELSE g % 40 END, g % 3, repeat('p', 4100)
FROM generate_series(1, 832) g;
-- an explicit cube with the measures; the default cube; no cube
CREATE TABLE ed (LIKE ec INCLUDING STORAGE) WITH (autovacuum_enabled = off);
CREATE TABLE eo (LIKE ec INCLUDING STORAGE) WITH (autovacuum_enabled = off);
INSERT INTO ed SELECT * FROM ec;
INSERT INTO eo SELECT * FROM ec;
CREATE INDEX ec_col ON ec USING sift_col (id, kind, grp, flag, amt, score, n2, n4, n8, cnt, extra)
	WITH (write_buffer_limit = 4096, preagg = 'cube(kind: amt, score, n2, n4, n8, cnt; flag: n2, cnt; grp: n4, n8)');
CREATE INDEX ed_col ON ed USING sift_col (id, kind, grp, flag, amt, score, n2, n4, n8, cnt, extra) WITH (write_buffer_limit = 4096);
CREATE INDEX eo_col ON eo USING sift_col (id, kind, grp, flag, amt, score, n2, n4, n8, cnt, extra) WITH (write_buffer_limit = 4096, preagg = 'off');
CALL pa_settle();
VACUUM (ANALYZE, FREEZE, DISABLE_PAGE_SKIPPING) ec, ed, eo;
SELECT sift.col_verify('ec_col', true);
-- 1. built, every page all-visible
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');
SELECT * FROM pa_sum('eo');
-- the answers of the first way, which the plain plan gives too (the rows of a query on one line)
SELECT n, replace(pa_rows(format(q, 'ec')), E'\n', ' / ') AS answer FROM pa_q WHERE n NOT IN (4) ORDER BY n;
-- the plan counts the units answered from the pre-aggregates
-- 2. a few pages change (update, delete): their units are scanned, the others answer
UPDATE ec SET n4 = -(n4 / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 10 AND 20;
DELETE FROM ec WHERE id BETWEEN 530 AND 540;
UPDATE ed SET n4 = -(n4 / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 10 AND 20;
DELETE FROM ed WHERE id BETWEEN 530 AND 540;
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');
-- 3. vacuumed: the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec, ed;
SELECT * FROM pa_check('ec');
-- 4. rows in the write buffer (a kind no unit holds), then sealed
INSERT INTO ec SELECT g, CASE WHEN g % 4 = 0 THEN NULL ELSE 7 + g % 3 END, 'g' || (g % 40), g % 2, g % 100, g % 9, g % 5, g % 7, g, g % 3, g % 11, repeat('p', 4100)
FROM generate_series(300001, 300300) g;
SELECT * FROM pa_check('ec');
SELECT kind, sum(n8), sum(n4) FROM ec WHERE kind >= 7 GROUP BY kind ORDER BY kind;
SELECT sift.col_promote('ec_col') >= 0 AS promoted;
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec;
SELECT * FROM pa_check('ec');
-- 5. merged: the dead rows are gone and every unit is stored with its pre-aggregates again
SELECT sift.col_merge('ec_col', true) >= 0 AS merged;
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec;
SELECT sift.col_verify('ec_col', true);
SELECT * FROM pa_check('ec');
-- 5b. an index written with three measures (an older cube) and then with the default: the units of the first kind are scanned for the other measures,
-- the later ones answer, and the answers do not change
ALTER INDEX ed_col SET (preagg = 'cube(kind: amt, score, id; flag: amt, score, id; grp: amt, score, id)');
INSERT INTO ed SELECT g + 500000, g % 7, 'g' || (g % 30), g % 2, g % 100, g % 9, g % 5, g % 7, g, g % 3, g % 11, repeat('p', 4100) FROM generate_series(1, 300) g;
SELECT sift.col_promote('ed_col') >= 0 AS promoted;
ALTER INDEX ed_col SET (preagg = 'default');
INSERT INTO ed SELECT g + 600000, g % 7, 'g' || (g % 30), g % 2, g % 100, g % 9, g % 5, g % 7, g, g % 3, g % 11, repeat('p', 4100) FROM generate_series(1, 300) g;
SELECT sift.col_promote('ed_col') >= 0 AS promoted;
VACUUM (freeze, index_cleanup on, disable_page_skipping) ed;
SELECT * FROM pa_check('ed', 1, 9);
-- 6. built by parallel workers (several segments), then read by the workers of the scan: each folds its share and the states merge
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_leader_participation = off;
REINDEX INDEX ec_col;
RESET parallel_leader_participation;
SELECT * FROM pa_check('ec');
UPDATE ec SET extra = extra WHERE id < 600;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('ec', w_only => true);
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
-- a transaction that deleted rows sees them gone, another does not until it commits
BEGIN;
DELETE FROM eo WHERE id BETWEEN 1 AND 20;
SELECT * FROM pa_sum('eo');
ROLLBACK;
SELECT * FROM pa_sum('eo');
-- the switch
DROP TABLE ec, ed, eo, pa_q;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m5 CASCADE;
-- <<< measures

DROP PROCEDURE pa_settle();
DROP FUNCTION pa_check(text, int, int, bool), pa_sum(text, int, int, bool), pa_rows(text), pa_plain(text), pa_exec(text);
DROP FUNCTION mig_same(text), mig_run(text, bool);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
