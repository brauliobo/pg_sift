-- Format 1 on the unified scan (sift.test_unified_agg): ORDER BY .. LIMIT reads the global order of the first sort column (segments and write buffer merged) and has the heap confirm each entry, so a
-- row an UPDATE, a DELETE, a rollback, a vacuum or a transaction still in progress changed is the row the snapshot sees. A copy of the table without an index is the oracle: every shape (ASC and DESC,
-- NULLS FIRST and LAST, LIMIT and OFFSET, ties, a qual) answers what it does in each state of the data.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET sift.test_create_format1 = on;
SET sift.test_unified_agg = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE t (id int PRIMARY KEY, a int, ts timestamptz, pad int);
INSERT INTO t SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 40 END, TIMESTAMPTZ '2026-01-01' + (i * 37 % 1000) * INTERVAL '1 minute', i FROM generate_series(1, 1000) i;
CREATE INDEX t_ix ON t USING sift_col (id, a, ts, pad) WITH (order_array_columns = 'a, ts', write_buffer_limit = 65536);
VACUUM (FREEZE, ANALYZE) t;
-- a second segment, then rows in the write buffer
INSERT INTO t SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 40 END, TIMESTAMPTZ '2026-01-01' + (i * 37 % 1000) * INTERVAL '1 minute', i FROM generate_series(1001, 1300) i;
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
INSERT INTO t SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 40 END, TIMESTAMPTZ '2026-01-01' + (i * 37 % 1000) * INTERVAL '1 minute', i FROM generate_series(1301, 1400) i;
SELECT count(*) FILTER (WHERE kind = 'buffer') AS buffers, count(*) FILTER (WHERE kind <> 'buffer') AS segments FROM sift.col_segments('t_ix');
CREATE TABLE p AS SELECT * FROM t;
ANALYZE t;
ANALYZE p;

-- the same statement on the table and on its copy
CREATE FUNCTION mirror(stmt text, tbl text DEFAULT 't', oracle text DEFAULT 'p') RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(stmt, '@', tbl);
	EXECUTE replace(stmt, '@', oracle);
END $$;

-- whether the rows of q ('@' stands for the table) are those of the copy, in order
CREATE FUNCTION same(q text, tbl text DEFAULT 't', oracle text DEFAULT 'p') RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(s::text, '','') FROM (%s) s', replace(q, '@', tbl)) INTO a;
	EXECUTE format('SELECT string_agg(s::text, '','') FROM (%s) s', replace(q, '@', oracle)) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs: % / %', q, left(a, 80), left(b, 80); END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

-- the node of the plan of q, and the entries of the global order a run of q decodes
CREATE FUNCTION node(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || replace(q, '@', 't') LOOP
		IF l ~ 'Custom Scan' THEN RETURN (regexp_match(l, '\((\w+)\)'))[1]; END IF;
	END LOOP;
	RETURN 'none';
END $$;
CREATE FUNCTION decoded(q text, tbl text DEFAULT 't') RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE n int8;
BEGIN
	PERFORM sift._test_gorder_decoded(true);
	EXECUTE format('SELECT count(*) FROM (%s) s', replace(q, '@', tbl)) INTO n;
	RETURN sift._test_gorder_decoded(true);
END $$;

-- the shapes: the sort column, its direction and NULLs, the rows skipped and kept, one sort column (the keys alone are compared: the rows that tie are the executor's choice) or two
CREATE TABLE shapes AS
SELECT col, dir, nl, lim, off, ties
FROM (VALUES ('a'), ('ts')) c(col), (VALUES ('ASC'), ('DESC')) d(dir), (VALUES ('NULLS FIRST'), ('NULLS LAST')) n(nl),
	 (VALUES (1, 0), (20, 0), (20, 30), (100, 700)) l(lim, off), (VALUES (false), (true)) t(ties);
CREATE FUNCTION shape_sql(s shapes) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN s.ties THEN format('SELECT id, %1$s FROM @ ORDER BY %1$s %2$s %3$s, id LIMIT %4$s OFFSET %5$s', s.col, s.dir, s.nl, s.lim, s.off)
		ELSE format('SELECT %1$s FROM @ ORDER BY %1$s %2$s %3$s LIMIT %4$s OFFSET %5$s', s.col, s.dir, s.nl, s.lim, s.off) END
$$;
-- a third of the shapes, by the stage: each stage another third, and with a qual on the first column or on another
CREATE FUNCTION sweep(stage int, tbl text DEFAULT 't', oracle text DEFAULT 'p') RETURNS TABLE (same_rows bigint, shapes bigint) LANGUAGE sql AS
$$ SELECT count(*) FILTER (WHERE same(shape_sql(s), tbl, oracle)), count(*) FROM shapes s WHERE (hashtext(s::text || stage)::bigint & 2147483647) % 3 = stage % 3 $$;
CREATE FUNCTION quals(stage int) RETURNS TABLE (same_rows bigint, queries bigint) LANGUAGE sql AS
$$ SELECT count(*) FILTER (WHERE same(q)), count(*) FROM (VALUES
	('SELECT id, a FROM @ WHERE pad > 300 AND a IS NOT NULL ORDER BY ts DESC, id LIMIT 20'),
	('SELECT id, ts FROM @ WHERE a > 10 ORDER BY ts, id LIMIT 15 OFFSET 5'),
	('SELECT id FROM @ WHERE a < 3 ORDER BY a DESC NULLS LAST, id LIMIT 25'),
	('SELECT id FROM @ WHERE id > 700 AND a = 7 ORDER BY ts DESC, id LIMIT 10'),
	('SELECT id FROM @ WHERE a IS NULL ORDER BY a, ts DESC, id LIMIT 12')) v(q) $$;

SELECT * FROM sweep(0);
SELECT * FROM quals(0);

-- the plan: the tournament node for one sort column, the rows node for more or with a qual, neither sorts: the global order is read, a few chunks of it
SELECT node(q) AS node, decoded(q) BETWEEN 1 AND 500 AS reads_the_head_of_the_order
FROM (VALUES ('SELECT id FROM @ ORDER BY ts DESC LIMIT 10'), ('SELECT id FROM @ ORDER BY a NULLS FIRST LIMIT 10'), ('SELECT id FROM @ ORDER BY ts, id LIMIT 10'),
	  ('SELECT id FROM @ WHERE a > 10 ORDER BY ts DESC, id LIMIT 10')) v(q);

-- a non-key update, a key update and a delete, committed
SELECT mirror('UPDATE @ SET pad = pad + 1 WHERE id % 7 = 0');
SELECT * FROM sweep(1);
SELECT mirror('UPDATE @ SET a = coalesce(a, 0) + 500, ts = ts + INTERVAL ''1 day'' WHERE id % 5 = 0');
SELECT * FROM sweep(2);
SELECT * FROM quals(2);
SELECT mirror('DELETE FROM @ WHERE id % 11 = 0');
SELECT * FROM sweep(3);

-- rolled back, in the transaction itself, and in a transaction that is still in progress (a prepared one: no session sees it, as one that has not committed)
BEGIN;
SELECT mirror('INSERT INTO @ SELECT i + 5000, i % 40, TIMESTAMPTZ ''2027-01-01'' + i * INTERVAL ''1 minute'', 0 FROM generate_series(1, 60) i');
SELECT mirror('UPDATE @ SET a = 9999, ts = ts + INTERVAL ''7 days'' WHERE id % 9 = 0');
SELECT mirror('DELETE FROM @ WHERE id % 4 = 0');
SELECT * FROM sweep(4);
SELECT * FROM quals(4);
ROLLBACK;
SELECT * FROM sweep(5);

BEGIN;
SELECT mirror('INSERT INTO @ SELECT i + 6000, i % 40, TIMESTAMPTZ ''2027-01-01'' + i * INTERVAL ''1 minute'', 0 FROM generate_series(1, 60) i');
SELECT mirror('UPDATE @ SET a = 9999, ts = ts + INTERVAL ''7 days'' WHERE id % 9 = 0');
SELECT mirror('UPDATE @ SET pad = pad + 3 WHERE id % 6 = 0');
SELECT mirror('DELETE FROM @ WHERE id % 4 = 0');
PREPARE TRANSACTION 'gorder_in_progress';
SELECT * FROM sweep(6);
SELECT * FROM quals(6);
SELECT node('SELECT id FROM @ ORDER BY a LIMIT 10') AS node, decoded('SELECT id FROM @ ORDER BY a LIMIT 10') BETWEEN 1 AND 500 AS reads_the_head_of_the_order;
ROLLBACK PREPARED 'gorder_in_progress';
SELECT * FROM sweep(7);

BEGIN;
SELECT mirror('INSERT INTO @ SELECT i + 7000, i % 40, TIMESTAMPTZ ''2027-01-01'' + i * INTERVAL ''1 minute'', 0 FROM generate_series(1, 60) i');
SELECT mirror('UPDATE @ SET a = 9999, ts = ts + INTERVAL ''7 days'' WHERE id % 9 = 0');
SELECT mirror('DELETE FROM @ WHERE id % 4 = 0');
PREPARE TRANSACTION 'gorder_committed_later';
SELECT * FROM sweep(8);
COMMIT PREPARED 'gorder_committed_later';
SELECT * FROM sweep(9);
SELECT * FROM quals(9);

-- vacuum: the units lose the dead rows (an entry of a row the segment lost is dropped), and an address a new row took holds that row
VACUUM t;
VACUUM p;
SELECT * FROM sweep(10);
SELECT mirror('UPDATE @ SET a = a + 1 WHERE id % 3 = 0');
SELECT mirror('INSERT INTO @ SELECT i + 8000, i % 40, TIMESTAMPTZ ''2027-02-01'' + i * INTERVAL ''1 minute'', 0 FROM generate_series(1, 200) i');
SELECT * FROM sweep(11);
SELECT * FROM quals(11);

-- the write buffer promoted: every row is in a segment
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
SELECT * FROM sweep(12);

-- the row at the last address of a table is deleted and vacuumed (its segment loses it), and a row that takes its address has the same key: the entry of the segment is the row's, which the heap
-- confirms; or another key: the entry is dropped and the row stands on its own
CREATE TABLE r (id int PRIMARY KEY, a int, ts timestamptz, pad int);
INSERT INTO r SELECT i, i % 40, TIMESTAMPTZ '2026-01-01' + i * INTERVAL '1 minute', i FROM generate_series(1, 1000) i;
CREATE INDEX r_ix ON r USING sift_col (id, a, ts, pad) WITH (order_array_columns = 'a, ts');
VACUUM (FREEZE, ANALYZE) r;
CREATE TABLE rp AS SELECT * FROM r;
ANALYZE rp;
CREATE FUNCTION recent() RETURNS boolean LANGUAGE sql AS $$
	SELECT same('SELECT id, a, ts FROM @ ORDER BY ts DESC, id LIMIT 5', 'r', 'rp') AND same('SELECT ts FROM @ ORDER BY ts DESC LIMIT 5', 'r', 'rp') AND same('SELECT id FROM @ ORDER BY a DESC, id LIMIT 5', 'r', 'rp')
$$;
SELECT ctid AS last_ctid, ts AS last_ts FROM r WHERE id = 1000 \gset
SELECT mirror('DELETE FROM @ WHERE id = 1000', 'r', 'rp');
VACUUM r;
VACUUM rp;
SELECT mirror(format('INSERT INTO @ VALUES (2000, 0, %L, 0)', :'last_ts'), 'r', 'rp');
SELECT ctid = :'last_ctid'::tid AS took_the_address FROM r WHERE id = 2000;
SELECT recent();
SELECT mirror('DELETE FROM @ WHERE id = 2000', 'r', 'rp');
VACUUM r;
VACUUM rp;
SELECT mirror(format('INSERT INTO @ VALUES (2001, 0, %L, 0)', :'last_ts'::timestamptz + INTERVAL '1 hour'), 'r', 'rp');
SELECT ctid = :'last_ctid'::tid AS took_the_address FROM r WHERE id = 2001;
SELECT recent();

-- a segment that keeps the ends of its order only (64 entries at each end of 1,000): a read of few rows is answered by them, one that goes past them reads the units, and the rows an end
-- names are confirmed all the same
SET sift.test_col_gorder_choice = 1;
SET sift.test_col_gorder_ends = 64;
CREATE TABLE e (id int PRIMARY KEY, a int, ts timestamptz, pad int);
INSERT INTO e SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 40 END, TIMESTAMPTZ '2026-01-01' + (i * 37 % 1000) * INTERVAL '1 minute', i FROM generate_series(1, 1000) i;
CREATE INDEX e_ix ON e USING sift_col (id, a, ts, pad) WITH (order_array_columns = 'a, ts', write_buffer_limit = 65536);
RESET sift.test_col_gorder_choice;
RESET sift.test_col_gorder_ends;
VACUUM (FREEZE, ANALYZE) e;
CREATE TABLE ep AS SELECT * FROM e;
ANALYZE ep;
SELECT decoded('SELECT id FROM @ ORDER BY ts DESC LIMIT 10', 'e') BETWEEN 1 AND 200 AS reads_the_end_of_the_order;
SELECT * FROM sweep(18, 'e', 'ep');
SELECT mirror('UPDATE @ SET a = coalesce(a, 0) + 500, ts = ts + INTERVAL ''1 day'' WHERE id % 5 = 0', 'e', 'ep');
SELECT mirror('DELETE FROM @ WHERE id % 11 = 0', 'e', 'ep');
SELECT * FROM sweep(19, 'e', 'ep');
SELECT mirror('INSERT INTO @ SELECT i + 5000, i % 40, TIMESTAMPTZ ''2025-01-01'' + i * INTERVAL ''1 minute'', 0 FROM generate_series(1, 30) i', 'e', 'ep');
SELECT * FROM sweep(20, 'e', 'ep');
VACUUM e;
VACUUM ep;
SELECT * FROM sweep(21, 'e', 'ep');
SELECT * FROM sweep(22, 'e', 'ep');

-- written again, and everything vacuumed: the units hold it all
SELECT mirror('INSERT INTO @ SELECT i + 9100, i % 40, TIMESTAMPTZ ''2028-01-01'' + i * INTERVAL ''1 minute'', 0 FROM generate_series(1, 50) i');
SELECT * FROM sweep(15);
VACUUM (FREEZE) t;
VACUUM (FREEZE) p;
SELECT * FROM sweep(16);
SELECT * FROM quals(16);
SELECT * FROM sweep(17);

DROP FUNCTION mirror, same, node, decoded, shape_sql, sweep, quals, recent;
DROP TABLE shapes, t, p, r, rp, e, ep;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
