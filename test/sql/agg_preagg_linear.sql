-- The sum and avg of an int8 column plus, minus or times a constant (`sum(n8 + 1)`, `avg(5 - n8)`, `sum(3 * n8)`) are added up from the stored sums of a unit
-- as `mul * sum + add * count` when the unit's zone proves that no value makes the arithmetic leave int8; a unit that may hold one is scanned, and the scan
-- raises "bigint out of range" as PostgreSQL's int8pl, int8mi and int8mul do. Every query runs three ways, which must give the same rows or the same error
-- (text, so the scale of the numeric sums): from the pre-aggregates, scanning every unit (sift.agg_preagg off), and the plain plan (sift.enable_agg off).
-- `pre` says how many units the first one answered from metadata.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

\ir test/agg_preagg_helpers.sql

\ir test/agg_preagg_units.sql

CREATE TABLE ev (id int, kind int, grp text, n8 bigint, m8 bigint, pad text) WITH (autovacuum_enabled = off);

CREATE TABLE pa_q (n int, shape text, q text);
INSERT INTO pa_q VALUES
	(1, 'plus',               'SELECT kind, sum(n8 + 1), avg(n8 + 1), count(n8) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(2, 'minus',              'SELECT kind, sum(n8 - 5) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(3, 'constant minus',     'SELECT kind, sum(5 - n8), avg(5 - n8) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(4, 'times',              'SELECT kind, sum(n8 * 3), sum(3 * n8), sum(n8 * -2) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(5, 'times zero, plus zero', 'SELECT kind, sum(n8 * 0), sum(n8 + 0) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(6, 'no key',             'SELECT sum(n8 + 1), avg(n8 - 1), sum(n8 * 7) FROM %s WHERE id <= 30000'),
	(7, 'text key',           'SELECT grp, sum(n8 + 100), sum(m8 + 1) FROM %s WHERE id <= 30000 GROUP BY grp'),
	(8, 'int4 and int2 constants', 'SELECT kind, sum(n8 + 2147483647), sum(n8 * 7::int2), sum(n8 - 32767::int2) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(9, 'a large factor',     'SELECT kind, sum(n8 * 1000000000000) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(10, 'where on the key',  'SELECT kind, sum(n8 + 1) FROM %s WHERE id <= 30000 AND kind IN (1, 3) GROUP BY kind'),
	(11, 'absent key',        'SELECT kind, sum(n8 + 1) FROM %s WHERE kind = 99 GROUP BY kind'),
	(12, 'NULL measures',     'SELECT kind, sum(n8 + 1), avg(n8 + 1), count(n8) FROM %s WHERE grp = ''g_null'' GROUP BY kind'),
	(13, 'the type',          'SELECT pg_typeof(sum(n8 + 1)), pg_typeof(avg(n8 * 2)) FROM %s WHERE id <= 10'),
	(14, 'int8 max + 1 raises', 'SELECT sum(n8 + 1) FROM %s'),
	(15, 'int8 min - 1 raises', 'SELECT kind, sum(n8 - 1) FROM %s GROUP BY kind'),
	(16, 'a product that raises', 'SELECT kind, sum(n8 * 1000000000000000) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(17, 'the zone avoids the extremes', 'SELECT sum(n8 + 1), avg(n8 + 1) FROM %s WHERE id < 30000'),
	(18, 'two leaves: scan',  'SELECT kind, sum(n8 + m8) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(19, 'two operations: scan', 'SELECT kind, sum((n8 + 1) * 2), sum(n8 + 1 + 2) FROM %s WHERE id <= 30000 GROUP BY kind'),
	(20, 'on the key leaf: scan', 'SELECT n8, sum(n8 + 1) FROM %s WHERE id < 100 GROUP BY n8');

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

-- n8: values of a few million with NULLs; every 1000th row of the ids 30001 to 40000 holds an int8 extreme, the rows of the group g_null have no n8
INSERT INTO ev
SELECT g, CASE WHEN g % 9 = 0 THEN NULL ELSE g % 7 END,
       CASE WHEN g % 31 = 0 THEN 'g_null' ELSE 'g' || lpad((g % 30)::text, 2, '0') END,
       CASE WHEN g % 31 = 0 OR g % 13 = 0 THEN NULL
            WHEN g > 30000 AND g % 1000 = 0 THEN 9223372036854775807
            WHEN g > 30000 AND g % 1000 = 500 THEN -9223372036854775807 - 1
            ELSE (g::bigint * 977) % 2000001 - 1000000 END,
       CASE WHEN g % 11 = 0 THEN NULL ELSE g % 1000 END, repeat('p', 200)
FROM generate_series(1, 40000) g;

-- an explicit cube with the measures; the default cube; no cube
CREATE TABLE ec (LIKE ev) WITH (autovacuum_enabled = off);
CREATE TABLE ed (LIKE ev) WITH (autovacuum_enabled = off);
CREATE TABLE eo (LIKE ev) WITH (autovacuum_enabled = off);
INSERT INTO ec SELECT * FROM ev;
INSERT INTO ed SELECT * FROM ev;
INSERT INTO eo SELECT * FROM ev;
DROP TABLE ev;
CREATE INDEX ec_col ON ec USING sift_col (id, kind, grp, n8, m8) WITH (write_buffer_limit = 4096, preagg = 'cube(kind: n8, m8; grp: n8, m8)');
CREATE INDEX ed_col ON ed USING sift_col (id, kind, grp, n8, m8) WITH (write_buffer_limit = 4096);
CREATE INDEX eo_col ON eo USING sift_col (id, kind, grp, n8, m8) WITH (write_buffer_limit = 4096, preagg = 'off');
CALL pa_settle();
VACUUM (ANALYZE, FREEZE, DISABLE_PAGE_SKIPPING) ec, ed, eo;
SELECT sift.col_verify('ec_col', true);

-- 1. built, every page all-visible
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');
SELECT * FROM pa_sum('eo');

-- the answers of the first way, which the plain plan gives too (the rows of a query on one line)
SELECT n, replace(pa_rows(format(q, 'ec'), true, true), E'\n', ' / ') AS answer FROM pa_q WHERE n NOT IN (7) ORDER BY n;

-- the plan counts the units answered from the pre-aggregates
SELECT units, pre FROM pa_units('SELECT kind, sum(n8 + 1), avg(n8 * 3) FROM ed WHERE id <= 30000 GROUP BY kind');

-- 2. a few pages change (update, delete): their units are scanned, the others answer
UPDATE ec SET n8 = -(n8 / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 100 AND 200;
DELETE FROM ec WHERE id BETWEEN 18000 AND 18100;
UPDATE ed SET n8 = -(n8 / 2), kind = (kind + 1) % 7 WHERE id BETWEEN 100 AND 200;
DELETE FROM ed WHERE id BETWEEN 18000 AND 18100;
SELECT * FROM pa_check('ec');
SELECT * FROM pa_sum('ed');

-- 3. vacuumed: the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) ec, ed;
SELECT * FROM pa_check('ec');

-- 4. rows in the write buffer (a kind no unit holds), then sealed
INSERT INTO ec SELECT g, CASE WHEN g % 4 = 0 THEN NULL ELSE 7 + g % 3 END, 'g' || (g % 40), g % 1000, g % 77, repeat('p', 200)
FROM generate_series(300001, 301500) g;
SELECT * FROM pa_check('ec');
SELECT kind, sum(n8 + 1), sum(n8 * 3) FROM ec WHERE kind >= 7 GROUP BY kind ORDER BY kind;
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
UPDATE ec SET m8 = m8 WHERE id < 3000;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('ec');
SELECT n, (u).workers > 0 AS workers, (u).pre > 0 AS some_pre
FROM (SELECT n, pa_units(format(q, 'ec')) AS u FROM pa_q WHERE n IN (1, 4, 6)) z ORDER BY n;
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
SELECT pre IS NULL AS off FROM pa_units('SELECT kind, sum(n8 + 1) FROM ed WHERE id <= 30000 GROUP BY kind');
RESET sift.agg_preagg;
SELECT pre > 0 AS on FROM pa_units('SELECT kind, sum(n8 + 1) FROM ed WHERE id <= 30000 GROUP BY kind');

DROP TABLE ec, ed, eo, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int);
DROP PROCEDURE pa_settle();
DROP EXTENSION pg_sift;
