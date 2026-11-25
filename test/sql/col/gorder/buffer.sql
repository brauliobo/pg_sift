-- A write buffer of many rows (here 4,500 against the 4,096 the order once gave up at) is sorted for the order like a segment's entries: an ordered read of a written index
-- stays on the global order, with the buffer's rows among the entries. Every ordered read answers what the table does (a copy without an index is the oracle): ASC and DESC,
-- NULLs, ties, LIMIT and OFFSET, bounds, then rows of the buffer deleted, updated, rolled back, and the buffer promoted.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SELECT setseed(0.42);

-- b spans the whole range of a bigint
CREATE TABLE t (id int, ts timestamp, d date, s smallint, b bigint, pad int);
INSERT INTO t SELECT i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', DATE '2026-01-01' + (random() * 300)::int,
	CASE WHEN i % 11 = 0 THEN NULL ELSE (random() * 30)::int END, CASE i % 5 WHEN 0 THEN 9223372036854775807 WHEN 1 THEN -9223372036854775808 ELSE ((random() * 2 - 1) * 9e18)::bigint END, 0 FROM generate_series(1, 3000) i;
CREATE INDEX t_ix ON t USING sift_col (id, ts, d, s, b, pad) WITH (order_array_columns = 'ts, d, s, b', write_buffer_limit = 65536);
VACUUM t;
INSERT INTO t SELECT 3000 + i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', DATE '2026-01-01' + (random() * 300)::int,
	CASE WHEN i % 11 = 0 THEN NULL ELSE (random() * 30)::int END, CASE i % 5 WHEN 0 THEN 9223372036854775807 WHEN 1 THEN -9223372036854775808 ELSE ((random() * 2 - 1) * 9e18)::bigint END, 0 FROM generate_series(1, 4500) i;
SELECT sum(rows) > 4096 AS buffer_over_the_old_limit FROM sift.col_segments('t_ix') WHERE kind = 'buffer';

CREATE TABLE p AS SELECT * FROM t;
ANALYZE t;
ANALYZE p;

CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.plan_reads_global_order(q) $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.same_keys_in_t_and_p(q) $$;
CREATE FUNCTION quant(col text, ty text, f float8) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT quote_literal(percentile_disc(%s) WITHIN GROUP (ORDER BY %I))||''::%s'' FROM p', f, col, ty) INTO r;
	RETURN r;
END $$;
CREATE TABLE shapes AS
SELECT col, ty, dir, nl, lim, off, lo, hi, quant(col, ty, 0.5) AS mid
FROM (VALUES ('ts', 'timestamp'), ('d', 'date'), ('s', 'smallint'), ('b', 'bigint')) c(col, ty),
	 (VALUES ('ASC'), ('DESC')) d(dir),
	 (VALUES ('NULLS FIRST'), ('NULLS LAST')) n(nl),
	 (VALUES (1), (20), (100)) l(lim),
	 (VALUES (0), (30), (700), (3000)) o(off),
	 (VALUES (NULL::float8), (0.01), (0.5), (0.98)) b1(lo),
	 (VALUES (NULL::float8), (0.02), (0.5), (0.99)) b2(hi)
WHERE (lo IS NULL OR hi IS NULL OR lo < hi) AND (off = 0 OR lim = 20);
CREATE FUNCTION shape_sql(s shapes) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT %1$s AS k FROM @ WHERE %2$s ORDER BY %1$s %3$s %4$s LIMIT %5$s OFFSET %6$s', s.col,
		concat_ws(' AND ', CASE WHEN s.lo IS NOT NULL THEN format('%s >= %s', s.col, quant(s.col, s.ty, s.lo)) END,
						   CASE WHEN s.hi IS NOT NULL THEN format('%s <= %s', s.col, quant(s.col, s.ty, s.hi)) END, 'true'),
		s.dir, s.nl, s.lim, s.off)
$$;

-- a sweep checks the shapes whose hash with the stage falls in a third of them: each stage another third
CREATE FUNCTION sweep(stage int) RETURNS TABLE (same bigint, shapes bigint) LANGUAGE sql AS
$$ SELECT count(*) FILTER (WHERE same(shape_sql(s))), count(*) FROM shapes s WHERE hashtext(s::text || stage) % 3 = 0 $$;
SELECT * FROM sweep(1);
CREATE FUNCTION on_the_order() RETURNS TABLE (range_from boolean, range_desc boolean, range_between boolean, offset_in_the_buffer_range boolean, nulls_first boolean) LANGUAGE sql AS $$
	SELECT global_order('SELECT ts AS k FROM t WHERE ts >= TIMESTAMP ''2026-04-01'' ORDER BY ts LIMIT 20'),
		   global_order('SELECT ts AS k FROM t WHERE ts <= TIMESTAMP ''2026-04-01'' ORDER BY ts DESC LIMIT 20'),
		   global_order('SELECT ts AS k FROM t WHERE ts >= TIMESTAMP ''2026-03-01'' AND ts < TIMESTAMP ''2026-03-02'' ORDER BY ts LIMIT 100'),
		   global_order('SELECT ts AS k FROM t WHERE ts >= TIMESTAMP ''2026-02-01'' ORDER BY ts LIMIT 20 OFFSET 700'),
		   global_order('SELECT s AS k FROM t ORDER BY s NULLS FIRST LIMIT 20')
$$;
SELECT * FROM on_the_order();
SELECT same('SELECT d AS k FROM @ ORDER BY d LIMIT 700') AS ties_asc, same('SELECT d AS k FROM @ ORDER BY d DESC LIMIT 700') AS ties_desc;

-- rows of the buffer deleted and updated (the update moves the key), an insert and an update rolled back
DELETE FROM t WHERE id > 3000 AND id % 7 = 0;
UPDATE t SET ts = ts + INTERVAL '3 days', s = s + 1 WHERE id > 3000 AND id % 11 = 0;
BEGIN;
UPDATE t SET ts = ts + INTERVAL '1 year' WHERE id > 3000 AND id % 3 = 0;
INSERT INTO t SELECT 40000 + i, TIMESTAMP '2026-01-01', DATE '2026-01-01', 1, 1, 1 FROM generate_series(1, 200) i;
ROLLBACK;
DROP TABLE p;
CREATE TABLE p AS SELECT * FROM t;
SELECT sum(rows) > 4096 AS buffer_over_the_old_limit FROM sift.col_segments('t_ix') WHERE kind = 'buffer';
SELECT * FROM sweep(2);
SELECT * FROM on_the_order();

-- the buffer promoted, then written again
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
INSERT INTO t SELECT 50000 + i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', DATE '2026-01-01' + (random() * 300)::int, (random() * 30)::int, (random() * 1000)::bigint, 0 FROM generate_series(1, 5000) i;
DROP TABLE p;
CREATE TABLE p AS SELECT * FROM t;
SELECT * FROM sweep(3);
SELECT * FROM on_the_order();
SELECT sift.col_verify('t_ix');

DROP FUNCTION on_the_order(), global_order(text), same(text), quant(text, text, float8), sweep(int), shape_sql(shapes);
DROP TABLE shapes, p, t;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
