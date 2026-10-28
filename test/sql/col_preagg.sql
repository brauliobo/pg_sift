-- Pre-aggregates of a unit (docs/COL-PREAGG.md): the exact sums of the integer leaves and the per-dimension histograms the segment writer computes,
-- checked against direct SQL aggregates over the same rows after a build, DML, VACUUM, merge and a parallel build. Only units that are the
-- single unit of their superblock and exact (clean, every page all-visible) are compared: the rule under which a consumer may use them.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SELECT setseed(0.42);

CREATE FUNCTION pa_sb(t tid) RETURNS int LANGUAGE sql IMMUTABLE AS $$ SELECT ((t::text::point)[0])::int / 256 $$;

-- units checked and mismatches of the aggregate of a leaf: nvals, nnull, min, max, sum, sumsq (integer leaves), the float sum (float leaves)
CREATE FUNCTION pa_leaf(idx regclass, tbl regclass, leaf text, typ text, withsum bool, fl bool, OUT checked bool, OUT bad bigint, OUT sumsq_null bigint)
LANGUAGE plpgsql AS $f$
DECLARE cond text := 'u.nvals = h.n AND u.nnull = h.n - h.nn'; r record;
BEGIN
	IF typ IS NOT NULL THEN
		cond := cond || format(' AND u.min::%1$s IS NOT DISTINCT FROM h.mn AND u.max::%1$s IS NOT DISTINCT FROM h.mx', typ);
	END IF;
	-- a sum of squares that left int128 is reported as NULL (and counted)
	IF withsum THEN cond := cond || ' AND u.sum IS NOT DISTINCT FROM h.s AND (u.sumsq IS NULL OR u.sumsq = h.q)'; END IF;
	IF fl THEN cond := cond || ' AND u.sum_float IS NOT DISTINCT FROM h.sf'; END IF;
	EXECUTE format($q$
		WITH u AS (SELECT count(*) OVER (PARTITION BY sb) AS nunits, * FROM sift.col_unit_agg(%1$L, %2$L)),
		     h AS (SELECT pa_sb(ctid) AS sb, count(*) AS n, count(%2$I) AS nn,
		                  %4$s AS mn, %5$s AS mx, %6$s AS s, %7$s AS q, %8$s AS sf
		           FROM %3$s GROUP BY 1)
		SELECT count(*) > 0 AS c, count(*) FILTER (WHERE NOT (%9$s)) AS b, count(*) FILTER (WHERE u.sumsq IS NULL AND u.sum IS NOT NULL) AS q
		FROM u JOIN h USING (sb) WHERE u.exact AND u.nunits = 1
	$q$, idx, leaf, tbl,
		CASE WHEN typ IS NULL THEN 'NULL::text' WHEN typ = 'bool' THEN format('bool_and(%I)', leaf) ELSE format('min(%I)', leaf) END,
		CASE WHEN typ IS NULL THEN 'NULL::text' WHEN typ = 'bool' THEN format('bool_or(%I)', leaf) ELSE format('max(%I)', leaf) END,
		CASE WHEN withsum THEN format('sum(%I)::numeric', leaf) ELSE 'NULL::numeric' END,
		CASE WHEN withsum THEN format('sum(%1$I::numeric * %1$I::numeric)', leaf) ELSE 'NULL::numeric' END,
		CASE WHEN fl THEN format('sum(%I::float8)', leaf) ELSE 'NULL::float8' END,
		cond) INTO r;
	checked := r.c;
	bad := r.b;
	sumsq_null := r.q;
END $f$;

-- the same for the histogram of a dimension and one measure (NULL: the counts only); dimx is the dimension as the key text
CREATE FUNCTION pa_hist(idx regclass, tbl regclass, dim text, dimx text, meas text, fl bool, OUT checked bool, OUT bad bigint, OUT stored bool)
LANGUAGE plpgsql AS $f$
DECLARE r record;
BEGIN
	EXECUTE format($q$
		WITH s AS (SELECT * FROM sift.col_unit_hist(%1$L, %2$L, %3$L)),
		     one AS (SELECT sb FROM sift.col_unit_agg(%1$L, %2$L) WHERE sb IN (SELECT sb FROM s) GROUP BY sb HAVING count(*) = 1 AND bool_and(exact)),
		     h AS (SELECT pa_sb(ctid) AS sb, (%5$s) AS key, count(*) AS n, count(%6$s) AS nn, sum(%6$s)::numeric AS s, sum((%6$s)::float8) AS sf
		           FROM %4$s GROUP BY 1, 2)
		SELECT (SELECT count(*) FROM one) > 0 AS c, count(*) FILTER (WHERE s.n IS DISTINCT FROM h.n OR (s.nn IS NOT NULL AND
		       (s.nn IS DISTINCT FROM h.nn OR (s.nn > 0 AND %7$s)))) AS b,
		       (SELECT count(*) FROM s) > 0 AS st
		FROM (SELECT * FROM s WHERE sb IN (SELECT sb FROM one)) s
		FULL JOIN (SELECT * FROM h WHERE sb IN (SELECT sb FROM one)) h ON s.sb = h.sb AND s.key IS NOT DISTINCT FROM h.key
	$q$, idx, dim, meas, tbl, dimx, coalesce(meas, '1'),
		CASE WHEN fl THEN 's.sum_float IS DISTINCT FROM h.sf' ELSE 's.sum IS DISTINCT FROM h.s' END) INTO r;
	checked := r.c;
	bad := r.b;
	stored := r.st;
END $f$;

CREATE TABLE pa (id bigint, i2 smallint, i4 int, i8 bigint, n numeric(12,2), f4 real, f8 float8, d date, ts timestamp, b bool, s text, u text, di int, pad text);
INSERT INTO pa
SELECT g,
       CASE WHEN random() < 0.1 THEN NULL ELSE (random() * 60000 - 30000)::smallint END,
       CASE WHEN random() < 0.1 THEN NULL ELSE (random() * 4e9 - 2e9)::bigint::int END,
       CASE WHEN random() < 0.1 THEN NULL ELSE ((random() - 0.5) * 1.8e19)::numeric::bigint END,
       CASE WHEN random() < 0.1 THEN NULL ELSE ((random() * 1e9)::bigint::numeric / 100)::numeric(12,2) END,
       CASE WHEN random() < 0.1 THEN NULL ELSE (round(random() * 400) * 0.25)::real END,
       CASE WHEN g % 20011 < 3 THEN (ARRAY['NaN', '-0', 'Infinity'])[1 + g % 3]::float8
            WHEN random() < 0.1 THEN NULL ELSE round(random() * 200) * 0.5 END,
       CASE WHEN random() < 0.1 THEN NULL ELSE date '2024-01-01' + (random() * 29)::int END,
       CASE WHEN random() < 0.1 THEN NULL ELSE timestamp '2023-01-01' + random() * interval '1000 days' END,
       CASE WHEN random() < 0.1 THEN NULL ELSE random() < 0.5 END,
       CASE WHEN random() < 0.05 THEN NULL ELSE 'v' || (random() * 14)::int END,
       md5(g::text),
       CASE WHEN random() < 0.1 THEN NULL ELSE (random() * 40)::int - 20 END,
       repeat('x', 200)
FROM generate_series(1, 60000) g;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) pa;

-- one battery: every leaf, every dimension with each kind of measure, the structure
CREATE FUNCTION pa_battery(idx regclass) RETURNS TABLE(what text, checked bool, bad bigint, sumsq_null bigint) LANGUAGE sql AS $$
	SELECT 'i2', * FROM pa_leaf(idx, 'pa', 'i2', 'int2', true, false)
	UNION ALL SELECT 'i4', * FROM pa_leaf(idx, 'pa', 'i4', 'int4', true, false)
	UNION ALL SELECT 'i8', * FROM pa_leaf(idx, 'pa', 'i8', 'int8', true, false)
	UNION ALL SELECT 'n', * FROM pa_leaf(idx, 'pa', 'n', 'numeric', true, false)
	UNION ALL SELECT 'f4', * FROM pa_leaf(idx, 'pa', 'f4', 'float4', false, true)
	UNION ALL SELECT 'f8', * FROM pa_leaf(idx, 'pa', 'f8', 'float8', false, true)
	UNION ALL SELECT 'd', * FROM pa_leaf(idx, 'pa', 'd', 'date', false, false)
	UNION ALL SELECT 'ts', * FROM pa_leaf(idx, 'pa', 'ts', 'timestamp', false, false)
	UNION ALL SELECT 'b', * FROM pa_leaf(idx, 'pa', 'b', 'bool', false, false)
	UNION ALL SELECT 's', * FROM pa_leaf(idx, 'pa', 's', NULL, false, false)
	UNION ALL SELECT 'di', * FROM pa_leaf(idx, 'pa', 'di', 'int4', true, false)
	UNION ALL SELECT 'hist s/n', c, b, NULL::bigint FROM pa_hist(idx, 'pa', 's', 's', 'n', false) h(c, b, st)
	UNION ALL SELECT 'hist s/f8', c, b, NULL::bigint FROM pa_hist(idx, 'pa', 's', 's', 'f8', true) h(c, b, st)
	UNION ALL SELECT 'hist di/n', c, b, NULL::bigint FROM pa_hist(idx, 'pa', 'di', 'di::text', 'n', false) h(c, b, st)
	UNION ALL SELECT 'hist d/f4', c, b, NULL::bigint FROM pa_hist(idx, 'pa', 'd', 'd::text', 'f4', true) h(c, b, st)
	UNION ALL SELECT 'hist b', c, b, NULL::bigint FROM pa_hist(idx, 'pa', 'b', $x$CASE WHEN b THEN 't' WHEN NOT b THEN 'f' END$x$, NULL, false) h(c, b, st)
$$;

-- The default: dictionary, integer, date and bool dimensions; the high-cardinality text leaf and the timestamp have no histogram
CREATE INDEX pa_col ON pa USING sift_col (i2, i4, i8, n, f4, f8, d, ts, b, s, u, di) WITH (dead_rewrite_pct = 10, compression = 'auto');
SELECT sift.col_verify('pa_col');
SELECT what, checked, bad, sumsq_null > 0 AS sumsq_ovf FROM pa_battery('pa_col');
SELECT (SELECT count(DISTINCT sb) FROM sift.col_unit_hist('pa_col', 'u')) < (SELECT count(*) FROM sift.col_unit_agg('pa_col', 'u')) AS u_capped,
       (SELECT count(DISTINCT sb) FROM sift.col_unit_hist('pa_col', 'ts')) < (SELECT count(*) FROM sift.col_unit_agg('pa_col', 'ts')) AS ts_capped,
       (SELECT count(DISTINCT sb) FROM sift.col_unit_hist('pa_col', 's')) = (SELECT count(*) FROM sift.col_unit_agg('pa_col', 's')) AS s_everywhere;
SELECT preagg_bytes > 0 AS bytes, preagg_units > 0 AS units, preagg_units <= units AS within FROM sift.col_index_info('pa_col');
-- every unit of a fresh index answers (the table was vacuumed with FREEZE)
SELECT count(*) FILTER (WHERE NOT exact) AS inexact, count(*) > 1 AS several FROM sift.col_unit_agg('pa_col', 'i4');
-- float sums seed with the first value: a group of -0 sums to -0, as float8pl does
CREATE TABLE pz (g text, f float8, m int);
INSERT INTO pz SELECT 'z', '-0'::float8, 1 FROM generate_series(1, 3);
INSERT INTO pz SELECT 'p', 0.5, 1 FROM generate_series(1, 3);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pz;
CREATE INDEX pz_col ON pz USING sift_col (g, f, m);
SELECT key, n, nn, sum_float::text FROM sift.col_unit_hist('pz_col', 'g', 'f') ORDER BY key;

-- DML: inserts (write buffer), updates and deletes make the touched units inexact, never wrong
INSERT INTO pa SELECT g, 1, 1, 1, 1, 1, 1, '2024-02-01', '2023-01-01', true, 'v1', md5(g::text), 1, repeat('x', 200) FROM generate_series(60001, 62000) g;
UPDATE pa SET i4 = i4 / 2, s = 'moved' WHERE id % 97 = 0 AND id < 20000;
DELETE FROM pa WHERE id % 89 = 0 AND id < 20000;
SELECT what, checked, bad, sumsq_null > 0 AS sumsq_ovf FROM pa_battery('pa_col');
SELECT count(*) FILTER (WHERE NOT exact) > 0 AS some_inexact FROM sift.col_unit_agg('pa_col', 'i4');
-- VACUUM makes the pages all-visible again but the units still hold the dead rows: still inexact where dirty
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pa;
SELECT what, checked, bad, sumsq_null > 0 AS sumsq_ovf FROM pa_battery('pa_col');
SELECT count(*) FILTER (WHERE NOT exact) > 0 AS dirty_inexact FROM sift.col_unit_agg('pa_col', 'i4');
-- promotion and a merge rewrite the units: the dead rows are gone and the new units carry new sums
SELECT sift.col_promote('pa_col') >= 0 AS promoted;
SELECT sift.col_merge('pa_col', true) >= 0 AS merged;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) pa;
SELECT sift.col_verify('pa_col');
SELECT what, checked, bad, sumsq_null > 0 AS sumsq_ovf FROM pa_battery('pa_col');
SELECT count(*) FILTER (WHERE NOT exact) AS inexact FROM sift.col_unit_agg('pa_col', 'i4');

-- parallel build and REINDEX
SET max_parallel_maintenance_workers = 3;
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = '64MB';
REINDEX INDEX pa_col;
SELECT segments > 1 AS workers_wrote_segments FROM sift.col_index_info('pa_col');
SELECT sift.col_verify('pa_col');
SELECT what, checked, bad, sumsq_null > 0 AS sumsq_ovf FROM pa_battery('pa_col');
RESET max_parallel_maintenance_workers;
RESET min_parallel_table_scan_size;

-- an explicit cube: exactly these dimensions and measures, integer measures included, no byte budget
CREATE INDEX pa_cube ON pa USING sift_col (i2, i4, i8, n, f8, d, b, s, di) WITH (preagg = 'cube(s: i2, i4, i8, n; di: i8, f8; d; b: n)');
SELECT sift.col_verify('pa_cube');
SELECT 'cube s/i8', c, b FROM pa_hist('pa_cube', 'pa', 's', 's', 'i8', false) h(c, b, st);
SELECT 'cube s/i2', c, b FROM pa_hist('pa_cube', 'pa', 's', 's', 'i2', false) h(c, b, st);
SELECT 'cube di/f8', c, b FROM pa_hist('pa_cube', 'pa', 'di', 'di::text', 'f8', true) h(c, b, st);
SELECT 'cube b/n', c, b FROM pa_hist('pa_cube', 'pa', 'b', $x$CASE WHEN b THEN 't' WHEN NOT b THEN 'f' END$x$, 'n', false) h(c, b, st);
SELECT count(*) FILTER (WHERE nn IS NULL) AS no_measure_rows FROM sift.col_unit_hist('pa_cube', 's', 'i8');
SELECT count(*) FILTER (WHERE nn IS NOT NULL) > 0 AS f8_is_not_a_measure_of_s FROM sift.col_unit_hist('pa_cube', 's', 'f8');
SELECT count(*) > 0 AS d_hist FROM sift.col_unit_hist('pa_cube', 'd');

-- off: the exact sums stay (40 bytes per integer leaf and unit), no histogram anywhere
CREATE INDEX pa_off ON pa USING sift_col (i2, i4, s, di) WITH (preagg = 'off');
SELECT sift.col_verify('pa_off');
SELECT (SELECT count(*) FROM sift.col_unit_hist('pa_off', 's')) AS hist_rows, preagg_units, preagg_bytes > 0 AS has_sums FROM sift.col_index_info('pa_off');
SELECT what, checked, bad, sumsq_null > 0 AS sumsq_ovf FROM (SELECT 'i4' AS what, * FROM pa_leaf('pa_off', 'pa', 'i4', 'int4', true, false)) x;

-- options
ALTER INDEX pa_off SET (preagg = 'default');
SELECT reloptions FROM pg_class WHERE relname = 'pa_off';
CREATE INDEX pa_bad1 ON pa USING sift_col (i2, s) WITH (preagg = 'cube(nosuch)');
CREATE INDEX pa_bad2 ON pa USING sift_col (i2, f8) WITH (preagg = 'cube(f8)');
CREATE INDEX pa_bad3 ON pa USING sift_col (i2, s) WITH (preagg = 'cube(i2: s)');
CREATE INDEX pa_bad4 ON pa USING sift_col (i2, s) WITH (preagg = 'sometimes');
CREATE INDEX pa_bad5 ON pa USING sift_col (i2, s) WITH (preagg = 'cube(i2: ');
SELECT * FROM sift.col_unit_agg('pa_col', 'nosuch');

DROP TABLE pa, pz;
DROP FUNCTION pa_battery(regclass), pa_hist(regclass, regclass, text, text, text, bool), pa_leaf(regclass, regclass, text, text, bool, bool), pa_sb(tid);
DROP EXTENSION pg_sift;
