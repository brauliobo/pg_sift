-- The order of a write buffer's rows (the sorted keys and addresses an ordered read walks) is kept by the backend with the batches it was made from (col/scan/gorder.c),
-- named by the state of the kept sets, and extended when rows are appended. Every read is checked against a copy of the table without an index, and the counters of the
-- kept orders (sift._test_col_gkept_stats) say which way the read went: found the order as it was, extended it, made it. The steps: a repeated read, rows appended inside the
-- range of the keys the order was cut for, rows with NULL keys, a key outside the range and more rows than twice the order's (made again), rows deleted and updated, an insert rolled back,
-- VACUUM (the set is dropped), a cursor that holds the order while the buffer grows and other orders push it out of the cache, an extension that fails, and a promotion. The last section is a LATERAL top-N
-- for 200 groups over a buffer of thousands of rows: the rows are put in groups once, so a full read of the buffer is made three times, not for every group.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;

CREATE TABLE t (id int, k int, v bigint, pad text DEFAULT repeat('x', 40));
INSERT INTO t (id, k, v) SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE (i * 7919) % 20011 END, (i * 104729) % 30011 FROM generate_series(1, 20000) i;
CREATE INDEX t_ix ON t USING sift_col (id, k, v) WITH (order_array_columns = 'k, v', write_buffer_limit = 65536);
VACUUM t;
CREATE TABLE p AS SELECT * FROM t;
ANALYZE t;
ANALYZE p;

-- a statement on both tables; @ is the table
CREATE FUNCTION on_both(sql text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(sql, '@', 't');
	EXECUTE replace(sql, '@', 'p');
END $$;
CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || replace(q, '@', 't') LOOP
		IF l ~ 'Global Order: true' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
-- the answer through the index is the copy's, and the order was read
CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(k::text || '':'' || id, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) s) s', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(k::text || '':'' || id, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) s) s', replace(q, '@', 'p')) INTO b;
	RETURN CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERS ' || q WHEN global_order(q) THEN 'order' ELSE 'units' END;
END $$;
-- the keys alone, in order: rows with equal keys (the NULLs) come in any order
CREATE FUNCTION same_keys(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(k::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) s) s', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(k::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (%s) s) s', replace(q, '@', 'p')) INTO b;
	RETURN CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERS ' || q WHEN global_order(q) THEN 'order' ELSE 'units' END;
END $$;
CREATE FUNCTION reads() RETURNS TABLE (shape text, result text) LANGUAGE sql AS $$
	SELECT 'asc', same('SELECT k, id FROM @ ORDER BY k, id LIMIT 60')
	UNION ALL SELECT 'desc', same('SELECT k, id FROM @ ORDER BY k DESC, id LIMIT 60')
	UNION ALL SELECT 'nulls first', same('SELECT k, id FROM @ ORDER BY k NULLS FIRST, id LIMIT 60')
	UNION ALL SELECT 'range', same('SELECT k, id FROM @ WHERE k >= 9000 AND k < 9300 ORDER BY k, id LIMIT 200')
	UNION ALL SELECT 'offset', same('SELECT k, id FROM @ ORDER BY k, id LIMIT 40 OFFSET 2500')
	UNION ALL SELECT 'above', same('SELECT k, id FROM @ WHERE k >= 1000000 ORDER BY k, id LIMIT 40')
	UNION ALL SELECT 'below', same('SELECT k, id FROM @ WHERE k < 0 ORDER BY k DESC, id LIMIT 40')
	UNION ALL SELECT 'nulls first, keys', same_keys('SELECT k FROM @ ORDER BY k NULLS FIRST LIMIT 300')
	UNION ALL SELECT 'nulls first desc, keys', same_keys('SELECT k FROM @ ORDER BY k DESC NULLS FIRST LIMIT 300')
$$;
-- the counters since the last call
CREATE TABLE last_stats AS SELECT * FROM sift._test_col_gkept_stats();
CREATE FUNCTION moves(what text) RETURNS TABLE (step text, found bigint, extended bigint, made bigint, dropped bigint) LANGUAGE plpgsql AS $$
DECLARE s record; l record;
BEGIN
	SELECT * INTO s FROM sift._test_col_gkept_stats();
	SELECT * INTO l FROM last_stats;
	UPDATE last_stats SET hits = s.hits, extends = s.extends, builds = s.builds, drops = s.drops;
	RETURN QUERY SELECT $1, s.hits - l.hits, s.extends - l.extends, s.builds - l.builds, s.drops - l.drops;
END $$;

-- the stored segment only: no buffer, no order to keep
SELECT * FROM reads();
SELECT * FROM moves('segment only');

-- rows only in the write buffer: the order is made once, then found
SELECT sift.col_verify('t_ix') IS NOT NULL AS verified;
SELECT on_both('INSERT INTO @ (id, k, v) SELECT 20000 + i, (i * 6007) % 20011, i FROM generate_series(1, 3000) i');
SELECT * FROM reads();
SELECT * FROM moves('buffer built');
SELECT * FROM reads();
SELECT * FROM moves('read again');

-- rows appended with keys inside the range: the order is extended, once for the five reads
SELECT on_both('INSERT INTO @ (id, k, v) SELECT 30000 + i, (i * 3571) % 20011, -i FROM generate_series(1, 400) i');
SELECT * FROM reads();
SELECT * FROM moves('400 rows appended');
SELECT on_both('INSERT INTO @ (id, k, v) SELECT -i, NULL, i FROM generate_series(1, 30) i');
SELECT * FROM reads();
SELECT * FROM moves('NULL keys appended');

-- a key above and a key below the keys the order was cut for: made again, each time
SELECT on_both('INSERT INTO @ (id, k, v) VALUES (32002, 9999999, 2)');
SELECT * FROM reads();
SELECT * FROM moves('a key above the range');
SELECT on_both('INSERT INTO @ (id, k, v) VALUES (32001, -5000, 1)');
SELECT * FROM reads();
SELECT * FROM moves('a key below the range');

-- more than twice the rows: made again
SELECT on_both('INSERT INTO @ (id, k, v) SELECT 40000 + i, (i * 4421) % 20011, i FROM generate_series(1, 9000) i');
SELECT * FROM reads();
SELECT * FROM moves('a buffer three times as big');

-- deleted rows and updated rows (the update appends the new row; the old record stays until VACUUM)
SELECT on_both('DELETE FROM @ WHERE id BETWEEN 20100 AND 20400');
SELECT on_both('UPDATE @ SET k = k + 3 WHERE id BETWEEN 30000 AND 30200');
SELECT * FROM reads();
SELECT * FROM moves('deleted and updated');
SELECT on_both('UPDATE @ SET k = k + 1 WHERE id BETWEEN 40000 AND 40050');
SELECT * FROM reads();
SELECT * FROM moves('updated again');

-- an insert that rolls back leaves records of dead tuples in the buffer
BEGIN;
INSERT INTO t (id, k, v) SELECT 50000 + i, (i * 11) % 20011, 0 FROM generate_series(1, 300) i;
ROLLBACK;
SELECT * FROM reads();
SELECT * FROM moves('rolled back');

-- the same transaction writes and reads: the rows it wrote are in the order of its own reads
BEGIN;
SELECT on_both('INSERT INTO @ (id, k, v) SELECT 60000 + i, (i * 13) % 20011, i FROM generate_series(1, 200) i');
SELECT * FROM reads();
SELECT on_both('INSERT INTO @ (id, k, v) SELECT 61000 + i, (i * 17) % 20011, i FROM generate_series(1, 200) i');
SELECT * FROM reads();
COMMIT;
SELECT * FROM moves('the writer reads');

-- VACUUM flags the records of removed tuples: the set is made again, so is the order
VACUUM t;
VACUUM p;
SELECT * FROM reads();
SELECT * FROM moves('after VACUUM');

-- a cursor holds the order while rows are appended and other orders (other leaf, other bounds) fill the cache: it finishes with the rows it started with
SELECT on_both('INSERT INTO @ (id, k, v) SELECT 70000 + i, (i * 19) % 20011, i FROM generate_series(1, 500) i');
SELECT * FROM reads();
CREATE TABLE expect AS SELECT row_number() OVER () AS ord, k, id FROM (SELECT k, id FROM p ORDER BY k, id LIMIT 300) s;
CREATE TABLE got (ord int, k int, id int);
DO $$
DECLARE
	c CURSOR FOR SELECT k, id FROM t ORDER BY k, id LIMIT 300;
	r record;
	n int := 0;
BEGIN
	OPEN c;
	FOR i IN 1..100 LOOP
		FETCH c INTO r;
		n := n + 1;
		INSERT INTO got VALUES (n, r.k, r.id);
	END LOOP;
	INSERT INTO t (id, k, v) SELECT 80000 + i, (i * 23) % 20011, i FROM generate_series(1, 400) i;
	PERFORM count(*) FROM (SELECT k, id FROM t ORDER BY k, id LIMIT 300) s;
	PERFORM count(*) FROM (SELECT v, id FROM t ORDER BY v, id LIMIT 20) s;
	PERFORM count(*) FROM (SELECT v, id FROM t WHERE v > 100 ORDER BY v DESC, id LIMIT 20) s;
	PERFORM count(*) FROM (SELECT v, id FROM t WHERE v > 200 ORDER BY v, id LIMIT 20) s;
	PERFORM count(*) FROM (SELECT k, id FROM t WHERE k > 300 ORDER BY k, id LIMIT 20) s;
	LOOP
		FETCH c INTO r;
		EXIT WHEN NOT FOUND;
		n := n + 1;
		INSERT INTO got VALUES (n, r.k, r.id);
	END LOOP;
	CLOSE c;
END $$;
SELECT (SELECT count(*) FROM got) = 300 AS all_rows,
	   NOT EXISTS (SELECT 1 FROM got g FULL JOIN expect e USING (ord) WHERE g.k IS DISTINCT FROM e.k OR g.id IS DISTINCT FROM e.id) AS as_at_the_start;
SELECT on_both('DELETE FROM @ WHERE id >= 80000');
SELECT found >= 0 AS counted FROM moves('cursor and other orders');

-- an extension that fails (after the new arrays are made) leaves the order as it was: the next read extends it
SELECT on_both('INSERT INTO @ (id, k, v) SELECT 90000 + i, (i * 29) % 20011, i FROM generate_series(1, 100) i');
SET sift.test_col_gkept_fail = on;
SELECT count(*) FROM (SELECT k, id FROM t ORDER BY k, id LIMIT 60) s;
RESET sift.test_col_gkept_fail;
SELECT * FROM reads();
SELECT * FROM moves('failed extension');

-- a promotion: the buffer is gone, the order with it
SELECT sift.col_verify('t_ix') IS NOT NULL AS verified;
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
SELECT * FROM reads();
SELECT * FROM moves('after the promotion');

CREATE TABLE wg AS SELECT i AS id, 1 + (i * 7919) % 250 AS g, timestamp '2024-01-01' + i * interval '37 seconds' AS ts FROM generate_series(1, 20000) i;
CREATE INDEX wg_i ON wg USING sift_col (id, g, ts) WITH (order_array_columns = 'ts', order_array_group = 'g');
CREATE TABLE wg_o AS SELECT n AS i, CASE WHEN n % 47 = 0 THEN NULL WHEN n % 41 = 0 THEN 5000 + n ELSE 1 + (n * 31) % 250 END AS g FROM generate_series(1, 200) n;
VACUUM (FREEZE, ANALYZE) wg, wg_o;
-- a LATERAL top-N for each of many groups: the rows of the buffer by group (col/scan/gmem.c), the orders made by a full read of the buffer are three for 200 probes, none for the next statement
SET sift.agg_force = on;
SET enable_seqscan = off;
SET enable_sort = off;

CREATE FUNCTION wg_rows(on_ bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', on_::text, true);
	SELECT string_agg(z::text, ',' ORDER BY z.i, z.ts) INTO r FROM (
		SELECT v.i, x.ts FROM wg_o v LEFT JOIN LATERAL (SELECT ts FROM wg WHERE g = v.g ORDER BY ts DESC LIMIT 2) x ON true
		UNION ALL SELECT v.i, (SELECT ts FROM wg WHERE g = v.g ORDER BY ts LIMIT 1) FROM wg_o v) z;
	RETURN r;
END $$;
-- the orders made by a full read of the buffer, and whether the rows are the heap's
CREATE FUNCTION wg_check(label text, OUT step text, OUT full_reads int, OUT same boolean) LANGUAGE plpgsql AS $$
DECLARE b0 bigint; a text; b text;
BEGIN
	step := label;
	SELECT builds INTO b0 FROM sift._test_col_gkept_stats();
	a := wg_rows(true);
	SELECT builds - b0 INTO full_reads FROM sift._test_col_gkept_stats();
	b := wg_rows(false);
	same := a = b;
END $$;

SELECT * FROM wg_check('no buffer');
INSERT INTO wg SELECT 20000 + n, CASE WHEN n % 97 = 0 THEN NULL ELSE 1 + (n * 7) % 240 END, CASE WHEN n % 89 = 0 THEN NULL ELSE timestamp '2025-01-01' + n * interval '41 seconds' END FROM generate_series(1, 3000) n;
SELECT * FROM wg_check('3,000 rows in the write buffer');
SELECT * FROM wg_check('the same buffer again');
DELETE FROM wg WHERE id % 13 = 0;
INSERT INTO wg SELECT 30000 + n, 1 + (n * 3) % 250, timestamp '2026-01-01' + n * interval '43 seconds' FROM generate_series(1, 500) n;
SELECT * FROM wg_check('rows deleted and appended');
SELECT * FROM wg_check('and again');
RESET enable_sort; RESET enable_seqscan; RESET sift.agg_force;
DROP FUNCTION wg_check(text);
DROP FUNCTION wg_rows(bool);
DROP TABLE wg, wg_o;

DROP FUNCTION on_both(text), global_order(text), same(text), same_keys(text), reads(), moves(text);
DROP TABLE t, p, last_stats, expect, got;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
