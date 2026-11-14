-- The order of a leaf the build chose ('auto: a, b') keeps the ends of a segment of many rows: the first and the last
-- sift.test_col_gorder_ends values (64 here, 4,096 otherwise), neither end splitting a run of equal keys, and the first NULL rows. ORDER BY .. LIMIT starts at an end and stops at its edge,
-- or reads the units; the rows are the heap's either way. The choice between the ends and the whole order is the zones' (sift_cgord_pay): the testing setting sift.test_col_gorder_choice forces it for the 768 shapes, the last part leaves it to the rule.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
-- plain chunks: the column format minors below tell the layouts apart (col_gorder_seek covers the seekable ones)
SET sift.test_limits = 'gord_seek_pct=0';
SELECT setseed(0.42);
SET sift.test_col_gorder_ends = 64;
SET sift.test_col_gorder_choice = 1;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE t (id int, ts timestamp, val int, dup int);
INSERT INTO t SELECT i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', CASE WHEN i % 7 = 0 THEN NULL ELSE (random() * 1000000)::int END, (random() * 20)::int FROM generate_series(1, 4000) i;
CREATE INDEX t_ix ON t USING sift_col (id, ts, val, dup) WITH (order_array_columns = 'auto: ts, val, dup', write_buffer_limit = 64);
INSERT INTO t SELECT 4000 + i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', CASE WHEN i % 7 = 0 THEN NULL ELSE (random() * 1000000)::int END, (random() * 20)::int FROM generate_series(1, 3000) i;
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
SELECT format, count(*) AS segments, sum(rows) AS rows FROM sift.col_segments('t_ix') WHERE kind = 'immutable' GROUP BY format ORDER BY format;
SELECT sift.col_verify('t_ix');
-- rows that stay in the write buffer: the order reads them from memory, in full
INSERT INTO t SELECT 7000 + i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', (random() * 1000000)::int, (random() * 20)::int FROM generate_series(1, 100) i;

-- the order of the table, the oracle: a copy without an index
CREATE TABLE p AS SELECT * FROM t;
ANALYZE t;
ANALYZE p;

-- does the plan of q read the order (EXPLAIN ANALYZE says Global Order), and does q give the rows of the copy?
CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.plan_reads_global_order(q) $$;
CREATE FUNCTION same(col text, q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(k::text, '','') FROM (%s) s', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(k::text, '','') FROM (%s) s', replace(q, '@', 'p')) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs: %', q, left(a, 80); END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

-- every shape: key, direction, NULLs, limit, offset, and the bounds at the ends, in the middle and in the gap between them
CREATE TABLE shapes AS
SELECT col, dir, nl, lim, off, lo, hi
FROM (VALUES ('ts'), ('val')) c(col),
	 (VALUES ('ASC'), ('DESC')) d(dir),
	 (VALUES ('NULLS FIRST'), ('NULLS LAST')) n(nl),
	 (VALUES (1), (20), (60), (100)) l(lim),
	 (VALUES (0), (30)) o(off),
	 (VALUES (NULL::numeric), (0.01), (0.5), (0.98)) b1(lo),
	 (VALUES (NULL::numeric), (0.02), (0.5), (0.99)) b2(hi)
WHERE lo IS NULL OR hi IS NULL OR lo < hi;
SELECT count(*) AS shapes FROM shapes;
CREATE FUNCTION shape_sql(s shapes) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT %1$s AS k FROM @ WHERE %2$s ORDER BY %1$s %3$s %4$s LIMIT %5$s OFFSET %6$s', s.col,
		concat_ws(' AND ', CASE WHEN s.lo IS NOT NULL THEN format('%s >= %s', s.col, CASE WHEN s.col = 'ts' THEN format('TIMESTAMP ''2026-01-01'' + %s * INTERVAL ''300 days''', s.lo) ELSE (s.lo * 1000000)::int::text END) END,
								  CASE WHEN s.hi IS NOT NULL THEN format('%s <= %s', s.col, CASE WHEN s.col = 'ts' THEN format('TIMESTAMP ''2026-01-01'' + %s * INTERVAL ''300 days''', s.hi) ELSE (s.hi * 1000000)::int::text END) END, 'true'),
		s.dir, s.nl, s.lim, s.off)
$$;
SELECT count(*) FILTER (WHERE same(s.col, shape_sql(s))) AS same, count(*) AS shapes FROM shapes s;
-- and the ends are what answers the shapes that start at one
SELECT count(*) FILTER (WHERE global_order(replace(shape_sql(s), '@', 't'))) AS from_the_order FROM shapes s WHERE lo IS NULL AND hi IS NULL AND lim <= 60 AND col = 'ts' AND (dir, nl) IN (('ASC', 'NULLS LAST'), ('DESC', 'NULLS FIRST'));
SELECT global_order('SELECT ts FROM t WHERE ts >= TIMESTAMP ''2026-04-01'' ORDER BY ts DESC LIMIT 20') AS a_range_at_the_end, global_order('SELECT ts FROM t WHERE ts BETWEEN TIMESTAMP ''2026-04-01'' AND TIMESTAMP ''2026-05-01'' ORDER BY ts LIMIT 20') AS a_range_in_the_gap;

-- ties: a column of 21 values has runs far longer than an end, so no end holds a whole one and the order is read from the units
SELECT sift.col_verify('t_ix');
SELECT same('dup', 'SELECT dup AS k FROM @ ORDER BY dup LIMIT 20') AS ties_asc, same('dup', 'SELECT dup AS k FROM @ ORDER BY dup DESC LIMIT 20') AS ties_desc;

-- deletes and updates: the dead rows of an end are skipped; too many of them and the scan reads the units
DELETE FROM t WHERE ts < TIMESTAMP '2026-01-04';
UPDATE t SET ts = ts + INTERVAL '400 days' WHERE ts > TIMESTAMP '2026-10-25';
DROP TABLE p;
CREATE TABLE p AS SELECT * FROM t;
SELECT count(*) FILTER (WHERE same(s.col, shape_sql(s))) AS same, count(*) AS shapes FROM shapes s;
DELETE FROM t WHERE ts < TIMESTAMP '2026-03-01';
DROP TABLE p;
CREATE TABLE p AS SELECT * FROM t;
SELECT count(*) FILTER (WHERE same(s.col, shape_sql(s))) AS same, count(*) AS shapes FROM shapes s;

-- VACUUM and a merge write the ends of what is left
VACUUM t;
SELECT sift.col_merge('t_ix', true) >= 0 AS merged;
SELECT sift.col_verify('t_ix');
SELECT count(*) FILTER (WHERE same(s.col, shape_sql(s))) AS same, count(*) AS shapes FROM shapes s;
SELECT format, count(*) AS segments FROM sift.col_segments('t_ix') WHERE kind = 'immutable' GROUP BY format ORDER BY format;

-- complete blocks (the list of the user, or the testing setting 2) answer the same shapes
SET sift.test_col_gorder_choice = 2;
REINDEX INDEX t_ix;
SELECT format, count(*) AS segments FROM sift.col_segments('t_ix') WHERE kind = 'immutable' GROUP BY format ORDER BY format;
SELECT count(*) FILTER (WHERE same(s.col, shape_sql(s))) AS same, count(*) AS shapes FROM shapes s;
RESET sift.test_col_gorder_choice;
-- a segment of few rows is written complete
CREATE TABLE small (id int, ts timestamp);
INSERT INTO small SELECT i, TIMESTAMP '2026-01-01' + i * INTERVAL '1 hour' FROM generate_series(1, 100) i;
CREATE INDEX small_ix ON small USING sift_col (id, ts) WITH (order_array_columns = 'auto: ts');
SELECT format, rows FROM sift.col_segments('small_ix') WHERE kind = 'immutable';
SELECT sift.col_verify('small_ix');
-- the rule: a bound in the middle of the values reads the units whose zone holds it, whatever key the units are cut by (a range across the keys reads them all); the whole order pays
-- when those cold blocks, at 64 kB of index each, outweigh its bytes. Values in insertion order need no order (the build leaves the column out: the zones prune), random values over
-- many units keep all of it, in an index cut by a key too (the ends there made a range across the keys 24 times the cold blocks of the whole order at 1,000,000 rows)
CREATE TABLE zone_o (id int, tenant_id int, ts timestamp, counter_a int, pad text DEFAULT repeat('x', 100));
INSERT INTO zone_o (id, tenant_id, ts, counter_a)
SELECT i, i / 2500, TIMESTAMP '2026-01-01' + i * INTERVAL '10 seconds', abs(hashint4(i + 7)) % 1000000 FROM generate_series(1, 250000) i;
CREATE TABLE zone_r (LIKE zone_o INCLUDING DEFAULTS);
INSERT INTO zone_r SELECT id, tenant_id, TIMESTAMP '2026-01-01' + (abs(hashint4(id)) % 20000000) * INTERVAL '1 second', counter_a FROM zone_o;
CREATE TABLE zone_s (LIKE zone_r INCLUDING DEFAULTS);
INSERT INTO zone_s SELECT * FROM zone_r;
ANALYZE zone_o;
ANALYZE zone_r;
ANALYZE zone_s;
CREATE INDEX zone_o_ix ON zone_o USING sift_col (id, ts, counter_a) WITH (key_cut = 'off');
CREATE INDEX zone_r_ix ON zone_r USING sift_col (id, ts, counter_a) WITH (key_cut = 'off');
CREATE INDEX zone_s_ix ON zone_s USING sift_col (tenant_id, id, ts, counter_a) WITH (key_cut = 'tenant_id');
SELECT reloptions FROM pg_class WHERE relname IN ('zone_o_ix', 'zone_r_ix', 'zone_s_ix') ORDER BY relname;
CREATE FUNCTION middle_from_the_order(tbl text, col text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE m text;
BEGIN
	EXECUTE format('SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY %I)::text FROM %I', col, tbl) INTO m;
	RETURN global_order(format('SELECT %1$I FROM %2$I WHERE %1$I >= %3$L ORDER BY %1$I LIMIT 20', col, tbl, m));
END $$;
-- the blocks each index holds (has_ends: format 40) and whether a scan with a bound in the middle is read from the order
SELECT t.tbl, c.col, coalesce((SELECT bool_or(format = 40) FROM sift.col_segments(t.tbl || '_ix') WHERE kind = 'immutable'), false) AS has_ends, middle_from_the_order(t.tbl, c.col) AS middle_from_the_order
FROM (VALUES ('zone_o'), ('zone_r'), ('zone_s')) t(tbl), (VALUES ('counter_a'), ('ts')) c(col)
ORDER BY 1, 2;
SELECT sift.col_verify('zone_s_ix');
DROP TABLE zone_o, zone_r, zone_s, shapes, small, p, t CASCADE;
DROP FUNCTION same(text, text), global_order(text), middle_from_the_order(text, text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift CASCADE;
