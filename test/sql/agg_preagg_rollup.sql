-- SiftAgg answers the units that are exact from the rollup of the elements of a list that the sift_col index stored with them (docs/COL-PREAGG.md section 2):
-- the distinct tuples of the low-cardinality leaves of the elements (dims) with their counts and the sums, minima and maxima of the measures. Any group keys
-- over the dims (one, two, three, a subset), any conjuncts on them (=, IN, <>, IS NULL, LIKE, OR across dims) and count, sum, avg, min, max of the measures
-- are folded from the tuples. Every query runs three ways, which must give the same rows: from the rollups, scanning every unit (sift.agg_preagg off), and
-- the plain plan (sift.enable_agg off). `pre` says how many units the first one answered from metadata: all, some, none ('-': no SiftAgg).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
-- these tests are about the rollup path: it answers whatever the cost of its tuples (agg_rollup_cost has its own test)
SET sift.agg_rollup_cost = 0;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE rl (id int, grp int, lines jsonb, tags jsonb, attrs jsonb, pad text) WITH (autovacuum_enabled = off);

CREATE FUNCTION pa_rows(q text, pre bool, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.agg_preagg', pre::text, true);
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;

-- units of the SiftAgg node, those it answered from the pre-aggregates and the dims it read (NULL: the plan has no SiftAgg, or the query fails)
CREATE FUNCTION pa_units(q text, OUT units int, OUT pre int, OUT dims text) LANGUAGE plpgsql AS $$
DECLARE l text; c text := '';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
		dims := coalesce(substring(l from 'Pre-aggregate Dims: (.*)$'), dims);
		IF l ~ '^\s*(Units|Pre-aggregates|Rollup Refresh)' THEN c := c || ', ' || btrim(l); END IF;
	END LOOP;
	-- the counters of the node (units, those answered from the pre-aggregates, those scanned by cost or lacking their rollup, the reason the pre-aggregates were not used) go to the server log with the query, not to the output
	RAISE LOG 'pa_units %: %', left(q, 120), substr(c, 3);
EXCEPTION WHEN OTHERS THEN
	units := NULL; pre := NULL; dims := NULL;
END $$;

CREATE TABLE pa_q (n int, shape text, q text);
INSERT INTO pa_q VALUES
	(1, 'one key',           'SELECT e.a, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1'),
	(2, 'two keys',          'SELECT e.a, e.b, count(*), sum(e.q) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1, 2'),
	(3, 'three keys',        'SELECT e.a, e.b, e.c, count(*), sum(e.q), avg(e.q) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1, 2, 3'),
	(4, 'subset: second',    'SELECT e.b, count(*), sum(e.q), avg(e.q) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1'),
	(5, 'subset: c then a',  'SELECT e.c, e.a, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1, 2'),
	(6, 'filter =',          'SELECT e.b, count(*), sum(e.q) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE e.a = ''aa'' GROUP BY 1'),
	(7, 'filter IN, IN',     'SELECT e.b, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE e.a IN (''aa'', ''ab'', ''zz'') AND e.c IN (0, 1) GROUP BY 1'),
	(8, 'filter <>',         'SELECT e.c, count(*), sum(e.q) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE e.a <> ''aa'' GROUP BY 1'),
	(9, 'filter IS NULL',    'SELECT e.a, count(*), count(e.q) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE e.c IS NULL GROUP BY 1'),
	(10, 'filter LIKE',      'SELECT e.b, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE e.a LIKE ''a%%'' GROUP BY 1'),
	(11, 'filter OR dims',   'SELECT e.b, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE e.a = ''ab'' OR e.c = 1 GROUP BY 1'),
	(12, 'filter int range', 'SELECT e.a, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE e.c >= 1 GROUP BY 1'),
	(13, 'min max int',      'SELECT e.a, min(e.q), max(e.q) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1'),
	(14, 'min max float',    'SELECT e.a, min(e.w), max(e.w) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1'),
	(15, 'sum avg float',    'SELECT e.a, e.b, round(sum(e.w)::numeric, 6), round(avg(e.w)::numeric, 6) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1, 2'),
	(16, 'int4 measure',     'SELECT e.a, sum(e.amt), avg(e.amt), min(e.amt), max(e.amt) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1'),
	(17, 'count of leaves',  'SELECT e.a, count(e.a), count(e.c), count(e.q), count(e.w), count(e.amt) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1'),
	(18, 'no key',           'SELECT count(*), sum(e.q), min(e.q), max(e.w), round(avg(e.w)::numeric, 6) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4)'),
	(19, 'no key, filters',  'SELECT count(*), sum(e.q) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE e.a = ''ab'' AND e.b <> ''bb'' AND e.c IS NOT NULL'),
	(20, 'zone and dims',    'SELECT e.a, e.c, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE d.id < 15000 AND e.b = ''ba'' GROUP BY 1, 2'),
	(21, 'having, limit',    'SELECT e.a, e.b, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1, 2 HAVING count(*) > 100 ORDER BY 3 DESC, 1, 2 LIMIT 4'),
	(22, 'array elements',   'SELECT e.v->>''a'', e.v->>''b'', count(*), sum((e.v->>''q'')::int) FROM %s d CROSS JOIN LATERAL jsonb_array_elements(d.lines) e(v) GROUP BY 1, 2'),
	(23, 'scalars',          'SELECT t, count(*) FROM %s d CROSS JOIN LATERAL jsonb_array_elements_text(d.tags) t GROUP BY 1'),
	(24, 'scalars: test',    'SELECT count(*) FROM %s d CROSS JOIN LATERAL jsonb_array_elements_text(d.tags) t WHERE t IN (''t3'', ''t4'')'),
	(25, 'map: key',         'SELECT s.key, count(*), count(s.value::float8), round(sum(s.value::float8)::numeric, 6), round(avg(s.value::float8)::numeric, 6) FROM %s d CROSS JOIN LATERAL jsonb_each_text(d.attrs) s GROUP BY 1'),
	(26, 'map: key filter',  'SELECT s.key, count(*), min(s.value::float8), max(s.value::float8) FROM %s d CROSS JOIN LATERAL jsonb_each_text(d.attrs) s WHERE s.key IN (''m1'', ''m2'', ''m9'') GROUP BY 1'),
	(27, 'map: no key',      'SELECT count(*), round(sum(s.value::float8)::numeric, 6) FROM %s d CROSS JOIN LATERAL jsonb_each_text(d.attrs) s WHERE s.key LIKE ''m%%'' AND s.key <> ''m0'''),
	(28, 'map: keys only',   'SELECT s.key, count(*) FROM %s d CROSS JOIN LATERAL jsonb_each(d.attrs) s GROUP BY 1'),
	-- not answered from the rollups: the scan of the units
	(29, 'row test: scan',   'SELECT e.a, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE d.grp = 3 GROUP BY 1'),
	(30, 'measure test',     'SELECT e.a, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) WHERE e.q > 3 GROUP BY 1'),
	(31, 'row key: scan',    'SELECT d.grp, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1'),
	(32, 'distinct: scan',   'SELECT e.a, count(DISTINCT d.id) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1'),
	(33, 'map: value test',  'SELECT s.key, count(*) FROM %s d CROSS JOIN LATERAL jsonb_each_text(d.attrs) s WHERE s.value::float8 > 3 GROUP BY 1'),
	(34, 'key not a dim',    'SELECT e.q, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8, amt int4) GROUP BY 1');

-- one row per query: whether the three ways agree, and how much the first answered from metadata
CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 34) RETURNS TABLE (n int, shape text, pre text, dims text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.pre IS NULL THEN '-' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END, u.dims,
	       pa_rows(z.q, true, true) IS NOT DISTINCT FROM pa_rows(z.q, false, true) AND pa_rows(z.q, false, true) IS NOT DISTINCT FROM pa_rows(z.q, false, false)
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pa_q WHERE n BETWEEN lo AND hi) z, LATERAL pa_units(z.q) u ORDER BY z.n
$$;

CREATE FUNCTION pa_sum(tbl text, lo int DEFAULT 1, hi int DEFAULT 34) RETURNS TABLE (queries bigint, differ bigint, all_pre bigint, some_pre bigint, no_pre bigint) LANGUAGE plpgsql AS $$
DECLARE state text;
BEGIN
	SELECT count(*), count(*) FILTER (WHERE NOT same), count(*) FILTER (WHERE pre = 'all'), count(*) FILTER (WHERE pre = 'some'),
	       count(*) FILTER (WHERE pre = 'none'), string_agg(n || ':' || pre, ' ' ORDER BY n)
	INTO queries, differ, all_pre, some_pre, no_pre, state FROM pa_check(tbl, lo, hi);
	-- the state of the run goes to the server log (kept by CI), not to the output: a count that differs from the expected one can be read there
	RAISE LOG 'pa_sum %: % | units exact % | queue % | snapshots %', tbl, state,
		(SELECT string_agg(segment || '.' || sb || '=' || exact::int, ' ' ORDER BY segment, sb) FROM sift.col_unit_agg((tbl || '_col')::regclass, 'id')),
		(SELECT count(*) FROM sift.col_rollup_queue()),
		(SELECT string_agg(backend_type || ' ' || backend_xmin, ', ') FROM pg_stat_activity
		 WHERE datname = current_database() AND pid <> pg_backend_pid() AND backend_xmin IS NOT NULL);
	RETURN NEXT;
END $$;

-- a snapshot held by another session of this database (the refresh worker rewriting a segment, one that is still ending) keeps the pages of rows written
-- since from becoming all-visible, so the units stay scanned. Wait for the conditions themselves: the queue drained and no such session.
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

-- absent, null and empty arrays, elements with missing and null members; quarter multiples keep the float sums exact in any order
INSERT INTO rl
SELECT g, g % 7,
       CASE WHEN g % 10 = 0 THEN NULL
            WHEN g % 10 = 1 THEN '[]'
            ELSE (SELECT jsonb_agg(CASE WHEN (g + e) % 13 = 0 THEN jsonb_build_object('q', e)
                                        ELSE jsonb_strip_nulls(o) END ORDER BY e)
                  FROM generate_series(1, g % 5 + 1) e,
                       LATERAL (SELECT jsonb_build_object('a', CASE WHEN (g + e) % 11 = 0 THEN NULL ELSE (ARRAY['aa', 'ab', 'ba', 'bb', 'ca'])[1 + (g + e) % 5] END,
                                                          'b', (ARRAY['ba', 'bb', 'bc'])[1 + (g * e) % 3],
                                                          'c', CASE WHEN (g + 2 * e) % 9 = 0 THEN NULL ELSE (g + e) % 3 END,
                                                          'q', CASE WHEN (g * e) % 17 = 0 THEN NULL ELSE (g * e) % 9 END,
                                                          'w', CASE WHEN (g + e) % 7 = 0 THEN NULL ELSE ((g * e) % 40) * 0.25 END,
                                                          'amt', CASE WHEN (g + e) % 6 = 0 THEN NULL ELSE (g * e) % 500 END) AS o) l) END,
       CASE WHEN g % 19 = 0 THEN NULL ELSE coalesce((SELECT jsonb_agg(CASE WHEN (g + e) % 8 = 0 THEN NULL ELSE 't' || (g + e) % 6 END) FROM generate_series(1, g % 4) e), '[]') END,
       CASE WHEN g % 23 = 0 THEN NULL ELSE coalesce((SELECT jsonb_object_agg('m' || (g + e) % 6, CASE WHEN (g + e) % 9 = 0 THEN NULL ELSE ((g * e) % 50) * 0.25 END) FROM generate_series(1, g % 5) e), '{}') END,
       repeat('p', 200)
FROM generate_series(1, 30000) g;

CREATE TABLE rd (LIKE rl) WITH (autovacuum_enabled = off);
CREATE TABLE ro (LIKE rl) WITH (autovacuum_enabled = off);
CREATE TABLE rx (LIKE rl) WITH (autovacuum_enabled = off);
INSERT INTO rd SELECT * FROM rl;
INSERT INTO ro SELECT * FROM rl;
INSERT INTO rx SELECT * FROM rl;
-- rl: the dims named; rd: the automatic rule; ro: no rollup; rx: only two of the three dims
CREATE INDEX rl_col ON rl USING sift_col (id, grp, lines, tags, attrs)
	WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8, amt int4}, tags[] text, attrs.* float8', write_buffer_limit = 4096,
	      preagg = 'cube(grp: id) rollup(lines[].a, lines[].b, lines[].c: lines[].q, lines[].w, lines[].amt; tags[]; attrs.*#key: attrs.*)');
CREATE INDEX rd_col ON rd USING sift_col (id, grp, lines, tags, attrs)
	WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8, amt int4}, tags[] text, attrs.* float8', write_buffer_limit = 4096);
CREATE INDEX ro_col ON ro USING sift_col (id, grp, lines, tags, attrs)
	WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8, amt int4}, tags[] text, attrs.* float8', write_buffer_limit = 4096, preagg = 'off');
CREATE INDEX rx_col ON rx USING sift_col (id, grp, lines, tags, attrs)
	WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8, amt int4}, tags[] text, attrs.* float8', write_buffer_limit = 4096,
	      preagg = 'rollup(lines[].a, lines[].b: lines[].q, lines[].w, lines[].amt)');
CALL pa_settle();
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) rl, rd, ro, rx;
SELECT sift.col_verify('rl_col', true), sift.col_verify('rd_col', true), sift.col_verify('rx_col', true);
SELECT units > 2 AS units, preagg_units = units AS all_with_cube FROM sift.col_index_info('rl_col');

-- 1. built, every page all-visible: the dims named
SELECT * FROM pa_check('rl');
-- the automatic rule, the index without rollups, the index with two of the three dims (a query that reads c is scanned)
SELECT * FROM pa_sum('rd');
SELECT * FROM pa_sum('ro');
SELECT * FROM pa_check('rx', 1, 5);

-- 2. a few pages change: their units are scanned, the others answer
UPDATE rl SET lines = '[{"a": "ab", "b": "bb", "c": 1, "q": 5, "w": 1.5}]' WHERE id BETWEEN 100 AND 200;
DELETE FROM rl WHERE id BETWEEN 18000 AND 18100;
UPDATE rd SET lines = '[{"a": "ab", "b": "bb", "c": 1, "q": 5, "w": 1.5}]' WHERE id BETWEEN 100 AND 200;
SELECT * FROM pa_check('rl');
SELECT * FROM pa_sum('rd');

-- 3. vacuumed: the units that lost rows stay scanned until a merge rewrites them
VACUUM (index_cleanup on, disable_page_skipping) rl, rd;
SELECT * FROM pa_sum('rl');

-- 4. rows in the write buffer, then sealed
INSERT INTO rl SELECT g, g % 7, jsonb_build_array(jsonb_build_object('a', 'ab', 'b', 'bb', 'c', g % 3, 'q', g % 9, 'w', g * 0.25), jsonb_build_object('a', 'zz', 'q', 1)),
       '["t1", null]', '{"m1": 2.5, "m7": null}', repeat('p', 200) FROM generate_series(300001, 301500) g;
SELECT * FROM pa_sum('rl');
SELECT sift.col_promote('rl_col') >= 0 AS promoted;
VACUUM (index_cleanup on, disable_page_skipping) rl;
SELECT * FROM pa_sum('rl');

-- 5. merged: the dead rows are gone and every unit is stored with its pre-aggregates again
SELECT sift.col_merge('rl_col', true) >= 0 AS merged;
VACUUM (index_cleanup on, disable_page_skipping) rl;
SELECT sift.col_verify('rl_col', true);
SELECT * FROM pa_check('rl');

-- 6. built by parallel workers (several segments)
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_leader_participation = off;
REINDEX INDEX rl_col;
RESET parallel_leader_participation;
SELECT segments > 1 AS parallel_segments FROM sift.col_index_info('rl_col');
SELECT sift.col_verify('rl_col', true);
SELECT * FROM pa_sum('rl');
RESET max_parallel_maintenance_workers;

-- 7. the index without rollups: every unit is scanned
SELECT * FROM pa_sum('ro');

DROP TABLE rl, rd, ro, rx, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int);
DROP PROCEDURE pa_settle();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
