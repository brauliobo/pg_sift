-- The default cube keeps up to eight measures of a dim (the first three decide which dims fit the unit's byte budget, the others come with a dim that is kept),
-- so the sum, avg and count of the integer columns beyond the first three answer from the histogram of the group key too.
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

CREATE TABLE ev (id int, kind int, grp text, flag int, amt numeric(12, 2), score float8, n2 int2, n4 int4, n8 bigint, cnt int4, extra int4, pad text) WITH (autovacuum_enabled = off);

CREATE TABLE pa_q (n int, shape text, q text);
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
	(14, 'zone on the id',    'SELECT kind, sum(n4) FROM %s WHERE id < 20000 GROUP BY kind'),
	(15, 'having, limit',     'SELECT kind, sum(n8) FROM %s GROUP BY kind HAVING sum(n8) > 0 ORDER BY 2 DESC LIMIT 3'),
	(16, 'a measure past the eighth: scan', 'SELECT kind, sum(extra) FROM %s GROUP BY kind'),
	(17, 'min, max: scan',    'SELECT kind, min(n4), max(n8) FROM %s GROUP BY kind'),
	(18, 'expression: scan',  'SELECT kind, sum(n4 + n2) FROM %s GROUP BY kind');

-- one row per query: whether the three ways agree, and how much the first answered from metadata
CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 20) RETURNS TABLE (n int, shape text, pre text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.pre IS NULL THEN '-' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END,
	       pa_rows(z.q, true, true) IS NOT DISTINCT FROM pa_rows(z.q, false, true) AND
	       pa_rows(z.q, false, true) IS NOT DISTINCT FROM pa_rows(z.q, false, false)
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pa_q WHERE n BETWEEN lo AND hi) z, LATERAL pa_units(z.q) u ORDER BY z.n
$$;

-- the same, summed up for a table whose answers need not be listed
CREATE FUNCTION pa_sum(tbl text, lo int DEFAULT 1, hi int DEFAULT 20) RETURNS TABLE (queries bigint, differ bigint, all_pre bigint, some_pre bigint, no_pre bigint) LANGUAGE plpgsql AS $$
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

-- kind: 0 to 6 (a ninth of the rows NULL) in every unit; flag: 0 or 1; n2, n4, n8, cnt: NULLs among the values, n8 up to the int8 extremes
INSERT INTO ev
SELECT g, CASE WHEN g % 9 = 0 THEN NULL ELSE g % 7 END,
       CASE WHEN g % 17 = 0 THEN NULL ELSE 'g' || lpad((g % 30)::text, 2, '0') END,
       g % 2, CASE WHEN g % 19 = 0 THEN NULL ELSE ((g::bigint * 37) % 100000)::numeric / 100 END,
       (g % 1000) / 7.0,
       CASE WHEN g % 13 = 0 THEN NULL ELSE (g % 200 - 100)::int2 END,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 37) % 100000 - 40000 END,
       CASE WHEN g % 23 = 0 THEN NULL WHEN g % 1000 = 7 THEN 9223372036854775807 WHEN g % 1000 = 8 THEN -9223372036854775807 - 1 ELSE (g::bigint * 977) % 1000000 END,
       CASE WHEN g % 9 <> 0 AND g % 7 = 6 THEN NULL ELSE g % 40 END, g % 3, repeat('p', 200)
FROM generate_series(1, 40000) g;

-- an explicit cube with the measures; the default cube; no cube
CREATE TABLE ec (LIKE ev) WITH (autovacuum_enabled = off);
CREATE TABLE ed (LIKE ev) WITH (autovacuum_enabled = off);
CREATE TABLE eo (LIKE ev) WITH (autovacuum_enabled = off);
INSERT INTO ec SELECT * FROM ev;
INSERT INTO ed SELECT * FROM ev;
INSERT INTO eo SELECT * FROM ev;
DROP TABLE ev;
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
SELECT n, replace(pa_rows(format(q, 'ec'), true, true), E'\n', ' / ') AS answer FROM pa_q WHERE n NOT IN (4) ORDER BY n;

-- the plan counts the units answered from the pre-aggregates
SELECT units, pre FROM pa_units('SELECT kind, sum(n8), avg(n4) FROM ed GROUP BY kind');

-- 2. a few pages change (update, delete): their units are scanned, the others answer
UPDATE ec SET n4 = -(n4 / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 100 AND 200;
DELETE FROM ec WHERE id BETWEEN 18000 AND 18100;
UPDATE ed SET n4 = -(n4 / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 100 AND 200;
DELETE FROM ed WHERE id BETWEEN 18000 AND 18100;
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');

-- 3. vacuumed: the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec, ed;
SELECT * FROM pa_check('ec');

-- 4. rows in the write buffer (a kind no unit holds), then sealed
INSERT INTO ec SELECT g, CASE WHEN g % 4 = 0 THEN NULL ELSE 7 + g % 3 END, 'g' || (g % 40), g % 2, g % 100, g % 9, g % 5, g % 7, g, g % 3, g % 11, repeat('p', 200)
FROM generate_series(300001, 301500) g;
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
INSERT INTO ed SELECT g + 500000, g % 7, 'g' || (g % 30), g % 2, g % 100, g % 9, g % 5, g % 7, g, g % 3, g % 11, repeat('p', 200) FROM generate_series(1, 6000) g;
SELECT sift.col_promote('ed_col') >= 0 AS promoted;
ALTER INDEX ed_col SET (preagg = 'default');
INSERT INTO ed SELECT g + 600000, g % 7, 'g' || (g % 30), g % 2, g % 100, g % 9, g % 5, g % 7, g, g % 3, g % 11, repeat('p', 200) FROM generate_series(1, 6000) g;
SELECT sift.col_promote('ed_col') >= 0 AS promoted;
VACUUM (freeze, index_cleanup on, disable_page_skipping) ed;
SELECT * FROM pa_check('ed', 1, 9);

-- 6. built by parallel workers (several segments), then read by the workers of the scan: each folds its share and the states merge
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_leader_participation = off;
REINDEX INDEX ec_col;
RESET parallel_leader_participation;
SELECT segments > 1 AS parallel_segments FROM sift.col_index_info('ec_col');
SELECT * FROM pa_check('ec');
UPDATE ec SET extra = extra WHERE id < 20000;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('ec');
SELECT n, (u).workers > 0 AS workers, (u).pre > 0 AS some_pre
FROM (SELECT n, pa_units(format(q, 'ec')) AS u FROM pa_q WHERE n IN (2, 3, 6)) z ORDER BY n;
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
SELECT pre IS NULL AS off FROM pa_units('SELECT kind, sum(n8) FROM ed GROUP BY kind');
RESET sift.agg_preagg;
SELECT pre > 0 AS on FROM pa_units('SELECT kind, sum(n8) FROM ed GROUP BY kind');

DROP TABLE ec, ed, eo, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int);
DROP PROCEDURE pa_settle();
DROP EXTENSION pg_sift;
