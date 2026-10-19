-- SiftAgg answers the units that are exact from the element cubes the sift_col index stored with them (docs/COL-PREAGG.md section 7): the groups of
-- the elements of an array of objects (jsonb_to_recordset, jsonb_array_elements) or of scalars, counted and summed per value of one of their leaves.
-- Every query runs three ways, which must give the same rows: from the pre-aggregates, scanning every unit (sift.agg_preagg off), and the plain
-- plan (sift.enable_agg off). `pre` says how many units the first one answered from metadata: all, some, none ('-': no SiftAgg).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE ae (id int, grp int, items jsonb, tags jsonb, pad text) WITH (autovacuum_enabled = off);

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

-- units of the SiftAgg node, and those it answered from the pre-aggregates (NULL: the plan has no SiftAgg, or the query fails)
CREATE FUNCTION pa_units(q text, OUT units int, OUT pre int, OUT workers int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
		workers := coalesce(substring(l from 'Workers Planned: (\d+)')::int, workers);
	END LOOP;
EXCEPTION WHEN OTHERS THEN
	units := NULL; pre := NULL; workers := NULL;
END $$;

CREATE TABLE pa_q (n int, shape text, q text);
INSERT INTO pa_q VALUES
	(1, 'key text',          'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(2, 'int measure',       'SELECT e.kind, count(*), sum(e.n), avg(e.n) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(3, 'float measure',     'SELECT e.kind, count(*), round(sum(e.score)::numeric, 6), round(avg(e.score)::numeric, 6) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(4, 'counts',            'SELECT e.kind, count(e.n), count(e.score), count(e.kind) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(5, 'key int',           'SELECT e.n, count(*), round(sum(e.score)::numeric, 6) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(6, 'test of key',       'SELECT count(*), sum(e.n) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE e.kind = ''k1'''),
	(7, 'key in list',       'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE e.kind IN (''k1'', ''k2'', ''k9'') GROUP BY 1'),
	(8, 'null key',          'SELECT count(*), round(avg(e.score)::numeric, 6) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE e.kind IS NULL'),
	(9, 'text prefix',       'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE e.kind LIKE ''k1%%'' GROUP BY 1'),
	(10, 'int compare',      'SELECT e.n, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE e.n > 3 GROUP BY 1'),
	(11, 'zone covers all',  'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE d.id >= 1 GROUP BY 1'),
	(12, 'zone covers some', 'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE d.id < 15000 GROUP BY 1'),
	(13, 'zone and test',    'SELECT e.kind, sum(e.n) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE d.id < 15000 AND e.kind <> ''k0'' GROUP BY 1'),
	(14, 'row test: scan',   'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE d.grp = 3 GROUP BY 1'),
	(15, 'other leaf: scan', 'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE e.n = 2 GROUP BY 1'),
	(16, 'two keys: scan',   'SELECT e.kind, e.n, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1, 2'),
	(17, 'min max: scan',    'SELECT e.kind, min(e.n), max(e.n) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(18, 'row key: scan',    'SELECT d.grp, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(19, 'no key: scan',     'SELECT count(*), sum(e.n) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8)'),
	(20, 'distinct: scan',   'SELECT e.kind, count(DISTINCT d.id) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(21, 'having, limit',    'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1 HAVING count(*) > 100 ORDER BY 2 DESC, 1 LIMIT 3'),
	(22, 'array elements',   'SELECT e.v->>''kind'', count(*), sum((e.v->>''n'')::int) FROM %s d CROSS JOIN LATERAL jsonb_array_elements(d.items) e(v) GROUP BY 1'),
	(23, 'scalars',          'SELECT t, count(*) FROM %s d CROSS JOIN LATERAL jsonb_array_elements_text(d.tags) t GROUP BY 1'),
	(24, 'scalars: test',    'SELECT count(*) FROM %s d CROSS JOIN LATERAL jsonb_array_elements_text(d.tags) t WHERE t = ''t3'''),
	(25, 'huge floats',      'SELECT e.kind, count(*), sum(e.score) > 1e119 FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(26, 'bad measure',      'SELECT e.kind, sum(e.n) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1');

-- one row per query: whether the three ways agree (the scan may refuse a column it cannot read, as it does without pre-aggregates), and how much the first answered from metadata
CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 25) RETURNS TABLE (n int, shape text, pre text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.pre IS NULL THEN '-' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END,
	       pa_rows(z.q, true, true) IS NOT DISTINCT FROM pa_rows(z.q, false, true) AND
	       (pa_rows(z.q, false, true) IS NOT DISTINCT FROM pa_rows(z.q, false, false) OR pa_rows(z.q, false, true) LIKE 'ERROR: sift: a column of the aggregate scan holds%')
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pa_q WHERE n BETWEEN lo AND hi) z, LATERAL pa_units(z.q) u ORDER BY z.n
$$;

-- the same, summed up for a table whose answers need not be listed
CREATE FUNCTION pa_sum(tbl text, lo int DEFAULT 1, hi int DEFAULT 25) RETURNS TABLE (queries bigint, differ bigint, all_pre bigint, some_pre bigint, no_pre bigint) LANGUAGE sql AS $$
	SELECT count(*), count(*) FILTER (WHERE NOT same), count(*) FILTER (WHERE pre = 'all'), count(*) FILTER (WHERE pre = 'some'),
	       count(*) FILTER (WHERE pre = 'none') FROM pa_check(tbl, lo, hi)
$$;

-- absent, null and empty arrays, objects with missing and null members; in fixed id ranges what a unit cannot hold: floats too large for the sum of
-- their squares (3000..3003), more than 256 distinct kinds in a unit (20000..20400)
INSERT INTO ae
SELECT g, g % 7,
       CASE WHEN g % 10 = 0 THEN NULL
            WHEN g % 10 = 1 THEN '[]'
            WHEN g BETWEEN 3000 AND 3003 THEN '[{"kind": "huge", "n": 1, "score": 1e120}]'
            ELSE (SELECT jsonb_agg(CASE WHEN (g + e) % 13 = 0 THEN jsonb_build_object('n', e)
                                        WHEN g BETWEEN 20000 AND 20400 THEN jsonb_build_object('kind', 'u' || g || '_' || e, 'n', e, 'score', e * 0.25)
                                        ELSE CASE WHEN (g + e) % 2 = 0 THEN jsonb_strip_nulls(o) ELSE o END END ORDER BY e)
                  FROM generate_series(1, g % 4 + 1) e,
                       LATERAL (SELECT jsonb_build_object('kind', CASE WHEN (g + e) % 11 = 0 THEN NULL ELSE 'k' || (g + e) % 5 END,
                                                          'n', CASE WHEN (g * e) % 17 = 0 THEN NULL ELSE (g * e) % 9 END,
                                                          'score', CASE WHEN (g + e) % 7 = 0 THEN NULL ELSE ((g * e) % 40) * 0.25 END) AS o) l) END,
       CASE WHEN g % 19 = 0 THEN NULL ELSE coalesce((SELECT jsonb_agg(CASE WHEN (g + e) % 8 = 0 THEN NULL ELSE 't' || (g + e) % 6 END) FROM generate_series(1, g % 4) e), '[]') END,
       repeat('p', 200)
FROM generate_series(1, 30000) g;

-- the default cube; an explicit cube; no cube; an index of the format before the element cubes (minor 3)
CREATE TABLE ac (LIKE ae) WITH (autovacuum_enabled = off);
CREATE TABLE ao (LIKE ae) WITH (autovacuum_enabled = off);
CREATE TABLE a3 (LIKE ae) WITH (autovacuum_enabled = off);
-- a string in an int path (ids 12000..12003): a unit that holds one has no histogram of n, the one of kind stays, and a scan of n is refused
CREATE TABLE ab (LIKE ae) WITH (autovacuum_enabled = off);
INSERT INTO ab SELECT id, grp, CASE WHEN id BETWEEN 12000 AND 12003 THEN '[{"kind": "k1", "n": "7", "score": 2}, {"kind": "k2", "n": 2, "score": 3}]'::jsonb ELSE items END, tags, pad
FROM ae WHERE id <= 14000;
INSERT INTO ac SELECT * FROM ae;
INSERT INTO ao SELECT * FROM ae;
INSERT INTO a3 SELECT * FROM ae;
CREATE INDEX ae_col ON ae USING sift_col (id, grp, items, tags) WITH (paths = 'items[]{kind text, n int4, score float8}, tags[] text', write_buffer_limit = 4096);
CREATE INDEX ac_col ON ac USING sift_col (id, grp, items, tags)
	WITH (paths = 'items[]{kind text, n int4, score float8}, tags[] text', write_buffer_limit = 4096,
	      preagg = 'cube(grp: id; items[].kind: items[].n, items[].score; items[].n: items[].score; tags[])');
CREATE INDEX ab_col ON ab USING sift_col (id, grp, items, tags) WITH (paths = 'items[]{kind text, n int4, score float8}, tags[] text', write_buffer_limit = 4096);
CREATE INDEX ao_col ON ao USING sift_col (id, grp, items, tags) WITH (paths = 'items[]{kind text, n int4, score float8}, tags[] text', write_buffer_limit = 4096, preagg = 'off');
SET sift.col_format_minor = 3;
CREATE INDEX a3_col ON a3 USING sift_col (id, grp, items, tags) WITH (paths = 'items[]{kind text, n int4, score float8}, tags[] text', write_buffer_limit = 4096);
RESET sift.col_format_minor;
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) ae, ac, ao, a3, ab;	-- waits for the cleanup lock: a page another session has pinned (the checkpointer) would stay not all-visible
SELECT sift.col_verify('ae_col', true), sift.col_verify('ac_col', true), sift.col_verify('a3_col', true);
SELECT units > 2 AS units, preagg_units = units AS all_with_cube FROM sift.col_index_info('ac_col');

-- 1. built, every page all-visible
SELECT * FROM pa_check('ac');
SELECT * FROM pa_check('ae');
SELECT * FROM pa_sum('ao');
SELECT * FROM pa_sum('a3');
SELECT * FROM pa_check('ab', 1, 1) UNION ALL SELECT * FROM pa_check('ab', 26, 26) ORDER BY 1;
SELECT count(DISTINCT sb) FILTER (WHERE nn IS NULL) AS units_without_n, count(DISTINCT sb) FILTER (WHERE nn IS NOT NULL) AS units_with_n
FROM sift.col_unit_hist('ab_col', 'items[].kind', 'items[].n');

-- 2. a few pages change (update, delete): their units are scanned, the others answer
UPDATE ac SET items = '[{"kind": "k9", "n": 5, "score": 1.5}]' WHERE id BETWEEN 100 AND 200;
DELETE FROM ac WHERE id BETWEEN 18000 AND 18100;
UPDATE ae SET items = '[{"kind": "k9", "n": 5, "score": 1.5}]' WHERE id BETWEEN 100 AND 200;
DELETE FROM ae WHERE id BETWEEN 18000 AND 18100;
UPDATE a3 SET items = '[{"kind": "k9", "n": 5, "score": 1.5}]' WHERE id BETWEEN 100 AND 200;
SELECT * FROM pa_check('ac');
SELECT * FROM pa_sum('ae');
SELECT * FROM pa_sum('a3');

-- 3. vacuumed (the index is vacuumed too: with few dead rows VACUUM would skip it): the units that lost rows stay scanned until a merge rewrites them
VACUUM (index_cleanup on, disable_page_skipping) ac, ae, a3;
SELECT * FROM pa_check('ac');

-- 4. rows in the write buffer, then sealed
INSERT INTO ac SELECT g, g % 7, jsonb_build_array(jsonb_build_object('kind', 'k' || g % 5, 'n', g % 9, 'score', g * 0.25), jsonb_build_object('kind', 'w' || g % 3, 'n', 1)),
       '["t1", null]', repeat('p', 200) FROM generate_series(300001, 301500) g;
SELECT * FROM pa_check('ac');
SELECT sift.col_promote('ac_col') >= 0 AS promoted;
VACUUM (index_cleanup on, disable_page_skipping) ac;
SELECT * FROM pa_check('ac');

-- 5. merged: the dead rows are gone and every unit is stored with its pre-aggregates again
SELECT sift.col_merge('ac_col', true) >= 0 AS merged;
VACUUM (index_cleanup on, disable_page_skipping) ac;
SELECT sift.col_verify('ac_col', true);
SELECT * FROM pa_check('ac');

-- 6. built by parallel workers (several segments)
SET max_parallel_maintenance_workers = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_leader_participation = off;
REINDEX INDEX ac_col;
RESET parallel_leader_participation;
SELECT segments > 1 AS parallel_segments FROM sift.col_index_info('ac_col');
SELECT sift.col_verify('ac_col', true);
SELECT * FROM pa_check('ac');
RESET max_parallel_maintenance_workers;

-- 7. a minor 3 index that gets minor 4 segments (rows inserted and sealed after the format changed): the units of each answer by what they hold
SET sift.col_format_minor = 3;
INSERT INTO a3 SELECT g, g % 7, jsonb_build_array(jsonb_build_object('kind', 'k' || g % 5, 'n', g % 9, 'score', g * 0.25)), '["t1"]', repeat('p', 200) FROM generate_series(400001, 405000) g;
SELECT sift.col_promote('a3_col') >= 0 AS promoted;
RESET sift.col_format_minor;
INSERT INTO a3 SELECT g, g % 7, jsonb_build_array(jsonb_build_object('kind', 'k' || g % 5, 'n', g % 9, 'score', g * 0.25)), '["t1"]', repeat('p', 200) FROM generate_series(410001, 415000) g;
SELECT sift.col_promote('a3_col') >= 0 AS promoted;
VACUUM (index_cleanup on, disable_page_skipping) a3;
SELECT string_agg(format::text, ',' ORDER BY gen) AS formats FROM sift._test_col_formats('a3_col');
SELECT sift.col_verify('a3_col', true);
SELECT * FROM pa_sum('a3');
REINDEX INDEX a3_col;
SELECT string_agg(format::text, ',' ORDER BY gen) AS formats FROM sift._test_col_formats('a3_col');
SELECT * FROM pa_sum('a3');

-- 8. the index without cubes: every unit is scanned
SELECT * FROM pa_sum('ao');

DROP TABLE ae, ac, ao, a3, ab, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
