-- SiftAgg answers the units that are exact from the element cubes the sift_col index stored with them (docs/COL-PREAGG.md sections 2 and 6): the groups of
-- the elements of an array of objects (jsonb_to_recordset, jsonb_array_elements) or of scalars, counted and summed per value of one of their leaves.
-- Every query runs three ways, which must give the same rows: from the pre-aggregates, scanning every unit (sift.agg_preagg off), and the plain
-- plan (sift.enable_agg off). `pre` says how many units the first one answered from metadata: all, some, none ('-': no SiftAgg).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
-- these tests are about the rollup path: it answers whatever the cost of its tuples (agg_rollup_cost has its own test)
SET sift.agg_rollup_cost = 0;
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

-- the refresh worker of a query that found a unit without its rollup holds a snapshot while it rewrites the segment: a VACUUM that runs meanwhile
-- cannot make the pages of rows written since all-visible, so the units stay scanned. Wait for the queue to drain before the statements that need them.
CREATE PROCEDURE pa_settle() LANGUAGE plpgsql AS $$
DECLARE deadline timestamptz := clock_timestamp() + interval '120 seconds';
BEGIN
	WHILE EXISTS (SELECT 1 FROM sift.col_rollup_queue()) LOOP
		IF clock_timestamp() > deadline THEN RAISE EXCEPTION 'the rollup queue did not drain'; END IF;
		PERFORM pg_sleep(0.01);
	END LOOP;
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
	(15, 'other leaf', 'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) WHERE e.n = 2 GROUP BY 1'),
	(16, 'two keys',   'SELECT e.kind, e.n, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1, 2'),
	(17, 'min max',    'SELECT e.kind, min(e.n), max(e.n) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(18, 'row key: scan',    'SELECT d.grp, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8) GROUP BY 1'),
	(19, 'no key',     'SELECT count(*), sum(e.n) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int, score float8)'),
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

-- Compare execution with the stored dimension intersection, independently of physical unit packing.
-- Function-local settings also restore the changes made by pa_rows on return.
CREATE FUNCTION pa_contract(idx regclass, q text, two_keys bool)
RETURNS TABLE (inventory bool, visible bool, used bool, same bool) LANGUAGE plpgsql
SET sift.agg_preagg = on SET sift.enable_agg = on SET enable_seqscan = off AS $$
DECLARE
	stored bigint; eligible bigint; planned int; answered int; l text; siftagg bool := false;
	a text; b text; c text;
BEGIN
	SELECT count(*) > 0 AND count(*) = (SELECT units FROM sift.col_index_info(idx)) AND
	       count(DISTINCT (segment, sb)) = count(*) AND
	       bool_and((segment IS NOT NULL AND sb IS NOT NULL AND rows > 0) IS TRUE),
	       bool_and(exact IS TRUE), count(*)
	INTO inventory, visible, stored FROM sift.col_unit_agg(idx, 'id');
	WITH kind AS (SELECT DISTINCT segment, sb FROM sift.col_unit_hist(idx, 'items[].kind') WHERE exact IS TRUE),
	     nums AS (SELECT DISTINCT segment, sb FROM sift.col_unit_hist(idx, 'items[].n') WHERE exact IS TRUE)
	SELECT count(*) INTO eligible FROM kind k
	WHERE NOT two_keys OR EXISTS (SELECT 1 FROM nums n WHERE (n.segment, n.sb) = (k.segment, k.sb));
	-- Capture counters before pa_rows changes either setting; missing counters must fail.
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		siftagg := siftagg OR l LIKE '%Custom Scan (SiftAgg)%';
		planned := coalesce(substring(l from '^\s*Units: (\d+)')::int, planned);
		answered := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, answered);
	END LOOP;
	used := (siftagg AND planned = stored AND answered = eligible) IS TRUE;
	a := pa_rows(q, true, true);
	b := pa_rows(q, false, true);
	c := pa_rows(q, false, false);
	same := (a IS NOT NULL AND b IS NOT NULL AND c IS NOT NULL AND
	         a NOT LIKE 'ERROR:%' AND b NOT LIKE 'ERROR:%' AND c NOT LIKE 'ERROR:%' AND a = b AND b = c) IS TRUE;
	inventory := inventory IS TRUE;
	visible := visible IS TRUE;
	RETURN NEXT;
END $$;

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
	      preagg = 'cube(grp: id) rollup(items[].kind, items[].n: items[].n, items[].score; tags[])');
CREATE INDEX ab_col ON ab USING sift_col (id, grp, items, tags) WITH (paths = 'items[]{kind text, n int4, score float8}, tags[] text', write_buffer_limit = 4096);
CREATE INDEX ao_col ON ao USING sift_col (id, grp, items, tags) WITH (paths = 'items[]{kind text, n int4, score float8}, tags[] text', write_buffer_limit = 4096, preagg = 'off');
SET sift.col_format_minor = 3;
CREATE INDEX a3_col ON a3 USING sift_col (id, grp, items, tags) WITH (paths = 'items[]{kind text, n int4, score float8}, tags[] text', write_buffer_limit = 4096);
RESET sift.col_format_minor;
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) ae, ac, ao, a3, ab;	-- waits for the cleanup lock: a page another session has pinned (the checkpointer) would stay not all-visible
SELECT sift.col_verify('ae_col', true), sift.col_verify('ac_col', true), sift.col_verify('a3_col', true);
SELECT units > 2 AS units, preagg_units = units AS all_with_cube FROM sift.col_index_info('ac_col');

-- 1. built, every page all-visible
-- Freeze automatic rollup selection before any query can enqueue a refresh of ae.
SET sift.col_rollup_on_demand = off;
SELECT * FROM pa_check('ac');
SELECT * FROM pa_check('ae') WHERE n <> 16;
SELECT c.* FROM pa_q q, LATERAL pa_contract('ae_col', format(q.q, 'ae'), true) c WHERE q.n = 16;
RESET sift.col_rollup_on_demand;
SELECT * FROM pa_sum('ao');
SELECT * FROM pa_sum('a3');
SELECT * FROM pa_check('ab', 1, 1) UNION ALL SELECT * FROM pa_check('ab', 26, 26) ORDER BY 1;
SELECT count(DISTINCT sb) FILTER (WHERE nn IS NULL) AS units_without_n, count(DISTINCT sb) FILTER (WHERE nn IS NOT NULL) AS units_with_n
FROM sift.col_unit_hist('ab_col', 'items[].kind', 'items[].n');

CALL pa_settle();
-- Small automatic cubes: both dimensions fit; 65 distinct n values exceed the 64-key preselection cap.
-- The only element leaves are kind and n; short text/small integers leave ample encoding room.
SET sift.col_rollup_on_demand = off;
CREATE TABLE ap (id int, items jsonb) WITH (autovacuum_enabled = off);
INSERT INTO ap SELECT 1, jsonb_agg(jsonb_build_object('kind', k, 'n', n) ORDER BY k, n)
FROM unnest(ARRAY['a', 'b']) k CROSS JOIN generate_series(1, 2) n;
CREATE INDEX ap_col ON ap USING sift_col (id, items) WITH (paths = 'items[]{kind text, n int4}');
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) ap;
SELECT count(*) = 4 AS elements, count(DISTINCT e.kind) = 2 AS kinds, count(DISTINCT e.n) = 2 AS nums
FROM ap d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int);
SELECT (SELECT units FROM sift.col_index_info('ap_col')) = 1 AS one_unit,
       (SELECT count(DISTINCT (segment, sb)) FROM sift.col_unit_hist('ap_col', 'items[].kind')) = 1 AS kind_selected,
       (SELECT count(DISTINCT (segment, sb)) FROM sift.col_unit_hist('ap_col', 'items[].n')) = 1 AS n_selected;
SELECT * FROM pa_contract('ap_col', 'SELECT e.kind, e.n, count(*) FROM ap d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int) GROUP BY 1, 2', true);
SELECT * FROM pa_contract('ap_col', 'SELECT e.kind, count(*) FROM ap d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int) GROUP BY 1', false);
TRUNCATE ap;
INSERT INTO ap SELECT 1, jsonb_agg(jsonb_build_object('kind', CASE WHEN n % 2 = 0 THEN 'a' ELSE 'b' END, 'n', n) ORDER BY n)
FROM generate_series(1, 65) n;
REINDEX INDEX ap_col;
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) ap;
SELECT count(*) = 65 AS elements, count(DISTINCT e.kind) = 2 AS kinds, count(DISTINCT e.n) = 65 AS nums
FROM ap d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int);
SELECT (SELECT units FROM sift.col_index_info('ap_col')) = 1 AS one_unit,
       (SELECT count(DISTINCT (segment, sb)) FROM sift.col_unit_hist('ap_col', 'items[].kind')) = 1 AS kind_selected,
       (SELECT count(*) FROM sift.col_unit_hist('ap_col', 'items[].n')) = 0 AS n_excluded;
SELECT * FROM pa_contract('ap_col', 'SELECT e.kind, e.n, count(*) FROM ap d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int) GROUP BY 1, 2', true);
SELECT * FROM pa_contract('ap_col', 'SELECT e.kind, count(*) FROM ap d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, n int) GROUP BY 1', false);
DROP TABLE ap;
RESET sift.col_rollup_on_demand;

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
CALL pa_settle();
VACUUM (index_cleanup on, disable_page_skipping) ac, ae, a3;
SELECT * FROM pa_check('ac');

-- 4. rows in the write buffer, then sealed
INSERT INTO ac SELECT g, g % 7, jsonb_build_array(jsonb_build_object('kind', 'k' || g % 5, 'n', g % 9, 'score', g * 0.25), jsonb_build_object('kind', 'w' || g % 3, 'n', 1)),
       '["t1", null]', repeat('p', 200) FROM generate_series(300001, 301500) g;
SELECT * FROM pa_check('ac');
SELECT sift.col_promote('ac_col') >= 0 AS promoted;
CALL pa_settle();
VACUUM (index_cleanup on, disable_page_skipping) ac;
SELECT * FROM pa_check('ac');

-- 5. merged: the dead rows are gone and every unit is stored with its pre-aggregates again
SELECT sift.col_merge('ac_col', true) >= 0 AS merged;
CALL pa_settle();
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

-- 7. a minor 3 index that gets default minor 6 segments (rows inserted and sealed after the format changed): the units of each answer by what they hold
SET sift.col_format_minor = 3;
INSERT INTO a3 SELECT g, g % 7, jsonb_build_array(jsonb_build_object('kind', 'k' || g % 5, 'n', g % 9, 'score', g * 0.25)), '["t1"]', repeat('p', 200) FROM generate_series(400001, 405000) g;
SELECT sift.col_promote('a3_col') >= 0 AS promoted;
RESET sift.col_format_minor;
INSERT INTO a3 SELECT g, g % 7, jsonb_build_array(jsonb_build_object('kind', 'k' || g % 5, 'n', g % 9, 'score', g * 0.25)), '["t1"]', repeat('p', 200) FROM generate_series(410001, 415000) g;
SELECT sift.col_promote('a3_col') >= 0 AS promoted;
CALL pa_settle();
VACUUM (index_cleanup on, disable_page_skipping) a3;
SELECT array_agg(format ORDER BY format) = ARRAY[3,3,3,6]::int4[] AS mixed_formats FROM sift._test_col_formats('a3_col');
SELECT sift.col_verify('a3_col', true);
SELECT * FROM pa_sum('a3');
REINDEX INDEX a3_col;
SELECT string_agg(format::text, ',' ORDER BY gen) AS formats FROM sift._test_col_formats('a3_col');
SELECT * FROM pa_sum('a3');

-- 8. the index without cubes: every unit is scanned
SELECT * FROM pa_sum('ao');

DROP TABLE ae, ac, ao, a3, ab, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int), pa_sum(text, int, int), pa_contract(regclass, text, bool);
DROP PROCEDURE pa_settle();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
