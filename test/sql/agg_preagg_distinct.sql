-- count(DISTINCT col) of a column the cube stores a histogram of is the union of the groups of the histograms of the units whose rows are all visible;
-- the other units are scanned and add their values to the same set.
-- Every query runs three ways, which must give the same rows: from the pre-aggregates, scanning every unit (sift.agg_preagg off), and the plain plan
-- (sift.enable_agg off). `pre` says how many units the first one answered from metadata.
CREATE EXTENSION pg_sift;
SET default_statistics_target = 10000;
SET sift.agg_dead_delta = 0;	-- a unit with a dead row is scanned here (col_dead_delta answers it net of the dead rows)
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
SET max_parallel_maintenance_workers = 0;

\ir test/agg_preagg_helpers.sql

\ir test/agg_preagg_units.sql

CREATE TABLE pa_q (n int, shape text, q text);
INSERT INTO pa_q VALUES
	(1, 'int column',               'SELECT count(DISTINCT ki) FROM %s'),
	(2, 'text column',              'SELECT count(DISTINCT kt) FROM %s'),
	(3, 'with count and sum',       'SELECT count(DISTINCT ki), count(*), sum(v) FROM %s'),
	(4, 'test of the column',       'SELECT count(DISTINCT ki) FROM %s WHERE ki > 3'),
	(5, 'text test',                'SELECT count(DISTINCT kt) FROM %s WHERE kt <> ''t_b'''),
	(6, 'IN list',                  'SELECT count(DISTINCT ki) FROM %s WHERE ki IN (1, 3, 5, 99)'),
	(7, 'IS NULL',                  'SELECT count(DISTINCT ki) FROM %s WHERE ki IS NULL'),
	(8, 'zone of another column',   'SELECT count(DISTINCT ki) FROM %s WHERE id < 15000'),
	(9, 'range cuts a unit',        'SELECT count(DISTINCT kt) FROM %s WHERE id BETWEEN 7000 AND 21000'),
	(10, 'no rows',                 'SELECT count(DISTINCT ki) FROM %s WHERE id > 100000000'),
	(11, 'the key of the group',    'SELECT ki, count(DISTINCT ki), count(*) FROM %s GROUP BY ki'),
	(12, 'two columns: scan',       'SELECT count(DISTINCT ki), count(DISTINCT kt) FROM %s'),
	(13, 'other key: scan',         'SELECT kt, count(DISTINCT ki) FROM %s GROUP BY kt'),
	(14, 'many values: scan',       'SELECT count(DISTINCT kb) FROM %s'),
	(15, 'FILTER: scan',            'SELECT count(DISTINCT ki) FILTER (WHERE ki > 2) FROM %s'),
	(16, 'expression: scan',        'SELECT count(DISTINCT ki + 1) FROM %s'),
	(17, 'test of another: scan',   'SELECT count(DISTINCT ki) FROM %s WHERE kt = ''t_a'''),
	(18, 'having',                  'SELECT count(DISTINCT kt) FROM %s HAVING count(DISTINCT kt) > 2'),
	(19, 'plain count beside',      'SELECT count(DISTINCT ki), count(ki), min(v), max(v) FROM %s');

CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 19) RETURNS TABLE (n int, shape text, pre text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.pre IS NULL THEN '-' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END,
	       pa_rows(z.q, true, true) IS NOT DISTINCT FROM pa_rows(z.q, false, true) AND
	       pa_rows(z.q, false, true) IS NOT DISTINCT FROM pa_rows(z.q, false, false)
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pa_q WHERE n BETWEEN lo AND hi) z, LATERAL pa_units(z.q) u ORDER BY z.n
$$;

CREATE FUNCTION pa_sum(tbl text, lo int DEFAULT 1, hi int DEFAULT 19) RETURNS TABLE (queries bigint, differ bigint, all_pre bigint, some_pre bigint, no_pre bigint) LANGUAGE sql AS $$
	SELECT count(*), count(*) FILTER (WHERE NOT same), count(*) FILTER (WHERE pre = 'all'), count(*) FILTER (WHERE pre = 'some'),
	       count(*) FILTER (WHERE pre = 'none') FROM pa_check(tbl, lo, hi)
$$;

-- ki: 12 values with NULLs and one value (77) in a single row; kt: 6 texts with NULLs; kb: 400 values (more than a unit's histogram holds); v: measure
CREATE TABLE dd (id int, ki int, kt text, kb int, v int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO dd
SELECT g,
       CASE WHEN g = 31000 THEN 77 WHEN g % 13 = 0 THEN NULL ELSE (g * 7) % 12 END,
       CASE WHEN g % 17 = 0 THEN NULL ELSE 't_' || chr(97 + (g * 5) % 6) END,
       (g * 31) % 400,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 37) % 1000 END,
       repeat('p', 200)
FROM generate_series(1, 40000) g;
CREATE TABLE dn (LIKE dd) WITH (autovacuum_enabled = off);
INSERT INTO dn SELECT * FROM dd;
CREATE INDEX dd_col ON dd USING sift_col (id, ki, kt, kb, v) WITH (write_buffer_limit = 4096);
CREATE INDEX dn_col ON dn USING sift_col (id, ki, kt, kb, v) WITH (write_buffer_limit = 4096, preagg = 'off');
CALL pa_settle();
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) dd, dn;
SELECT sift.col_verify('dd_col', true);

-- a bounded differential fuzz (fixed seed): n queries made from hashes, each from the pre-aggregates against the plain plan
CREATE FUNCTION pa_fuzz(tbl text, seed int, n int) RETURNS TABLE (queries int, differ int, first_different text) LANGUAGE plpgsql AS $$
DECLARE
	h int[];
	q text;
	bad int := 0;
	first text[] := '{}';
BEGIN
	FOR i IN 1..n LOOP
		h := ARRAY(SELECT hashint4(seed * 100003 + i * 31 + x) & 2147483647 FROM generate_series(1, 8) x);
		q := format('SELECT %s FROM %s %s',
			(ARRAY['count(DISTINCT ki)', 'count(DISTINCT kt)', 'count(DISTINCT ki), count(*)', 'count(DISTINCT kt), sum(v), count(v)', 'count(DISTINCT ki), count(ki)'])[1 + h[1] % 5],
			tbl,
			(ARRAY['', 'WHERE ki > ' || h[2] % 12, 'WHERE ki < ' || h[2] % 12 || ' OR ki IS NULL', 'WHERE kt >= ''t_' || chr(97 + h[2] % 6) || '''',
			       'WHERE id BETWEEN ' || h[2] % 20000 || ' AND ' || (h[2] % 20000 + h[3] % 20000), 'WHERE ki IN (' || h[2] % 12 || ', ' || h[3] % 12 || ')',
			       'WHERE kt IS NULL', 'WHERE id < ' || h[2] % 40000 || ' AND ki <> ' || h[3] % 12])[1 + h[4] % 8]);
		IF pa_rows(q, true, true) IS DISTINCT FROM pa_rows(q, false, false) THEN
			bad := bad + 1;
			IF bad <= 3 THEN first := first || q; END IF;
		END IF;
	END LOOP;
	RETURN QUERY SELECT n, bad, array_to_string(first, E'\n');
END $$;

-- 1. built, every page all-visible
SELECT * FROM pa_check('dd');
SELECT * FROM pa_fuzz('dd', 1, 60);
SELECT * FROM pa_sum('dn');

-- 2. the only row of a value is changed and a few pages are deleted from: their units are scanned, the others answer
UPDATE dd SET ki = 5 WHERE id = 31000;
DELETE FROM dd WHERE id BETWEEN 18000 AND 18100;
DELETE FROM dd WHERE kt = 't_f' AND id < 9000;
SELECT * FROM pa_check('dd');
SELECT * FROM pa_fuzz('dd', 2, 60);

-- 3. vacuumed: the units that lost rows stay scanned until a merge rewrites them
VACUUM (freeze, index_cleanup on, disable_page_skipping) dd;
SELECT * FROM pa_check('dd');

-- 4. rows in the write buffer carry a value no unit has, then are sealed
INSERT INTO dd SELECT g, 50 + g % 3, 't_z', g % 400, g, repeat('p', 200) FROM generate_series(300001, 300500) g;
SELECT * FROM pa_check('dd');
SELECT sift.col_promote('dd_col') >= 0 AS promoted;
VACUUM (freeze, index_cleanup on, disable_page_skipping) dd;
SELECT * FROM pa_check('dd');

-- 5. merged: the dead rows are gone and every unit is stored with its pre-aggregates again
SELECT sift.col_merge('dd_col', true) >= 0 AS merged;
VACUUM (freeze, index_cleanup on, disable_page_skipping) dd;
SELECT sift.col_verify('dd_col', true);
SELECT * FROM pa_check('dd');
SELECT * FROM pa_fuzz('dd', 3, 60);

-- 6. built by parallel workers (several segments), then read by the workers of the scan: each adds its units' values and the sets merge
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_leader_participation = off;
REINDEX INDEX dd_col;
RESET parallel_leader_participation;
SELECT count(*) FILTER (WHERE NOT same) AS differ FROM pa_check('dd');
SET max_parallel_maintenance_workers = 0;
REINDEX INDEX dd_col;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM pa_sum('dd');
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;

-- the switch: off, the scan answers
SET sift.agg_preagg_distinct = off;
SELECT pre IS NULL AS off FROM pa_units('SELECT count(DISTINCT ki) FROM dd');
RESET sift.agg_preagg_distinct;
SELECT pre > 0 AS on FROM pa_units('SELECT count(DISTINCT ki) FROM dd');

DROP TABLE dd, dn, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int), pa_fuzz(text, int, int);
DROP PROCEDURE pa_settle();
DROP EXTENSION pg_sift;
