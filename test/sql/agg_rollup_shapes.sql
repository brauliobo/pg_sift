-- Which shapes of an analytics query over the elements of a list the rollups of the units answer, and the reason when one does not (EXPLAIN (ANALYZE, VERBOSE)
-- names it). Every shape runs three ways that must agree: from the rollups, scanning every unit (sift.agg_preagg off) and the plain plan (sift.enable_agg off).
-- `pre` says how many of the units the first one answered from the rollups: all, some or none ('-': the node does not use them).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE ev (id int, grp int, items jsonb, pad text) WITH (autovacuum_enabled = off);
INSERT INTO ev
SELECT g, g % 7,
       (SELECT jsonb_agg(jsonb_build_object('kind', CASE WHEN (g * e) % 17 = 0 THEN NULL ELSE 'k' || (g * e) % 5 END, 'flag', (ARRAY['fa', 'fb', 'fc'])[1 + (g + e) % 3], 'band', CASE WHEN (g + e) % 13 = 0 THEN NULL ELSE 1 + (g * 7 + e * 3) % 10 END,
                                            'seen', '2026-0' || (1 + (g + e) % 3) || '-1' || (e % 5), 'score', ((g * e) % 40) * 0.25, 'qty', (g * e) % 9) ORDER BY e)
        FROM generate_series(1, g % 5 + 1) e),
       repeat('p', 200)
FROM generate_series(1, 12000) g;
CREATE INDEX ev_col ON ev USING sift_col (id, grp, items)
	WITH (paths = 'items[]{kind text, flag text, band int4, seen text, score float8, qty int4}', write_buffer_limit = 4096,
	      rollup = 'items[].kind, items[].flag, items[].band, items[].seen');
VACUUM (ANALYZE, DISABLE_PAGE_SKIPPING) ev;
CREATE TABLE ev_dim AS SELECT 'k' || k AS kind, 'L' || (k % 2) AS label FROM generate_series(0, 4) k;
ANALYZE ev_dim;

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

-- the units of the SiftAgg node, those it answered from the rollups, and the line of the verbose plan that names why it did not (NULL: no SiftAgg node)
CREATE FUNCTION pa_units(q text, OUT units int, OUT pre int, OUT why text) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.agg_preagg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
		why := coalesce(substring(l from '^\s*Pre-aggregates: (.*)$'), why);
	END LOOP;
END $$;

CREATE TABLE pa_q (n int, shape text, q text);
INSERT INTO pa_q VALUES
	(1, 'one key',            'SELECT e.kind, count(*), sum(e.qty), avg(e.score) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(2, 'two keys, filter',   'SELECT e.kind, e.flag, count(*), min(e.score), max(e.qty) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) WHERE e.band >= 4 GROUP BY 1, 2'),
	(3, 'row range: zones',   'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) WHERE d.id <= 5000 GROUP BY 1'),
	(4, 'count distinct',     'SELECT e.kind, count(DISTINCT d.id) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(5, 'filtered counts',    'SELECT e.kind, count(*) FILTER (WHERE e.band >= 8), count(*) FILTER (WHERE e.band <= 6), count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(6, 'key: month of seen', 'SELECT left(e.seen, 7), count(*), avg(e.score) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(7, 'key: band case',     'SELECT CASE WHEN e.band >= 8 THEN ''hi'' WHEN e.band <= 6 THEN ''lo'' ELSE ''mid'' END, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(8, 'test on a measure',  'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) WHERE e.score > 5 GROUP BY 1'),
	(9, 'sum of expression',  'SELECT e.kind, sum(e.score * 2) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(10, 'grouping sets',     'SELECT e.kind, e.flag, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY ROLLUP (1, 2)'),
	(11, 'joined table',      'SELECT m.label, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) JOIN ev_dim m ON m.kind = e.kind GROUP BY 1'),
	(12, 'key of the rows',   'SELECT d.grp, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(13, 'variance',          'SELECT e.kind, var_pop(e.score) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(14, 'key: a measure',    'SELECT e.qty, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(15, 'row set test',      'SELECT e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) WHERE d.grp IN (1, 3, 5) GROUP BY 1'),
	(16, 'key: coalesce',     'SELECT coalesce(e.kind, ''none''), count(*), sum(e.qty) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1'),
	(17, 'key: band / 4',     'SELECT e.band / 4, e.flag, count(*), avg(e.score) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) GROUP BY 1, 2'),
	(18, 'key: lut and leaf, filter', 'SELECT upper(e.kind), e.band, min(e.score), max(e.score) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) WHERE e.flag <> ''fb'' GROUP BY 1, 2'),
	(19, 'key: month, filter on it', 'SELECT left(e.seen, 7), e.kind, count(*) FROM %s d CROSS JOIN LATERAL jsonb_to_recordset(d.items) e(kind text, flag text, band int4, seen text, score float8, qty int4) WHERE left(e.seen, 7) = ''2026-02'' GROUP BY 1, 2');

CREATE FUNCTION pa_check(tbl text, lo int DEFAULT 1, hi int DEFAULT 99) RETURNS TABLE (n int, shape text, pre text, why text, same bool) LANGUAGE sql AS $$
	SELECT z.n, z.shape, CASE WHEN u.pre IS NULL THEN '-' WHEN u.pre = 0 THEN 'none' WHEN u.pre = u.units THEN 'all' ELSE 'some' END, u.why,
	       pa_rows(z.q, true, true) IS NOT DISTINCT FROM pa_rows(z.q, false, true) AND pa_rows(z.q, false, true) IS NOT DISTINCT FROM pa_rows(z.q, false, false)
	FROM (SELECT n, shape, format(q, tbl) AS q FROM pa_q WHERE n BETWEEN lo AND hi) z, LATERAL pa_units(z.q) u ORDER BY z.n
$$;

SELECT sift.col_verify('ev_col', true);
SELECT * FROM pa_check('ev');

-- rows change: the units they are on are scanned, the others answer; the derived keys follow
UPDATE ev SET items = '[{"kind": "k1", "flag": "fa", "band": 9, "seen": "2026-02-10", "score": 1.5, "qty": 2}]' WHERE id BETWEEN 100 AND 150;
DELETE FROM ev WHERE id BETWEEN 7000 AND 7100;
SELECT n, shape, pre, why, same FROM pa_check('ev', 1, 7) UNION ALL SELECT n, shape, pre, why, same FROM pa_check('ev', 16, 19) ORDER BY 1;
VACUUM (index_cleanup on, disable_page_skipping) ev;
SELECT n, shape, pre, why, same FROM pa_check('ev', 1, 7) UNION ALL SELECT n, shape, pre, why, same FROM pa_check('ev', 16, 19) ORDER BY 1;

-- rows in the write buffer, then sealed
INSERT INTO ev SELECT g, g % 7, '[{"kind": null, "flag": "fc", "band": null, "seen": "2026-03-01", "score": 2.5, "qty": 3}, {"kind": "k2", "flag": "fa", "band": 4, "seen": "2026-01-02"}]', repeat('p', 200)
FROM generate_series(20001, 20800) g;
SELECT n, shape, pre, why, same FROM pa_check('ev', 1, 7) UNION ALL SELECT n, shape, pre, why, same FROM pa_check('ev', 16, 19) ORDER BY 1;
SELECT sift.col_promote('ev_col') >= 0 AS promoted;
VACUUM (index_cleanup on, disable_page_skipping) ev;
SELECT n, shape, pre, why, same FROM pa_check('ev', 1, 7) UNION ALL SELECT n, shape, pre, why, same FROM pa_check('ev', 16, 19) ORDER BY 1;
SELECT sift.col_verify('ev_col', true);

-- merged: the dead rows are gone and every unit is stored with its rollup again
SELECT sift.col_merge('ev_col', true) >= 0 AS merged;
VACUUM (index_cleanup on, disable_page_skipping) ev;
SELECT sift.col_verify('ev_col', true);
SELECT n, shape, pre, why, same FROM pa_check('ev', 1, 7) UNION ALL SELECT n, shape, pre, why, same FROM pa_check('ev', 16, 19) ORDER BY 1;

DROP TABLE ev, ev_dim, pa_q;
DROP FUNCTION pa_rows(text, bool, bool), pa_units(text), pa_check(text, int, int);
DROP EXTENSION pg_sift;
