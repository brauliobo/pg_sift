-- The rows of a statement or a transaction that a backend buffers before it writes them to the write buffer of a sift_col index: whatever the way a
-- row got into the transaction (one statement of many rows, one by one, COPY, an update's new version, ON CONFLICT, a savepoint that rolled back, a
-- second index), the transaction reads it at once through the pushdown, the index scan and the catalog functions of the index, and the table is the same
-- after COMMIT and after ROLLBACK. DDL on the index or the table in the transaction drops the rows that the index no longer holds.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET sift.enable_agg = on;
SET client_min_messages = warning;

CREATE TABLE w (id int PRIMARY KEY, k int, v numeric(10,2), s text) WITH (autovacuum_enabled = false);
INSERT INTO w SELECT g, g % 7, g % 97, 'a' || g % 5 FROM generate_series(1, 300) g;
CREATE INDEX w_col ON w USING sift_col (id, k, v, s) WITH (write_buffer_limit = 64);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) w;

-- the pushdown, the index scan and the heap agree on the table and on a filter; and the index lists the rows of the table (exactly them, when it was
-- built after the rows or rebuilt from them: a row written twice would show)
CREATE FUNCTION agree(tag text, exact bool DEFAULT false, idx text DEFAULT 'w_col') RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	qs text[] := ARRAY['SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (SELECT k, count(*), sum(v), count(DISTINCT s), max(id) FROM w GROUP BY k) x',
	                   'SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (SELECT id, k, v FROM w WHERE k = 3 OR id > 400) x'];
	q text; a text; b text; c text; bad text := '';
	idxrows bigint; heaprows bigint;
BEGIN
	FOREACH q IN ARRAY qs LOOP
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE q INTO a;
		PERFORM set_config('sift.enable_agg', 'off', true);
		PERFORM set_config('sift.enable_col_scan', 'on', true);
		PERFORM set_config('enable_seqscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		EXECUTE q INTO b;
		PERFORM set_config('sift.enable_col_scan', 'off', true);
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_indexonlyscan', 'off', true);
		EXECUTE q INTO c;
		PERFORM set_config('enable_indexscan', 'on', true);
		PERFORM set_config('enable_indexonlyscan', 'on', true);
		PERFORM set_config('enable_bitmapscan', 'on', true);
		PERFORM set_config('sift.enable_col_scan', 'on', true);
		PERFORM set_config('sift.enable_agg', 'on', true);
		bad := bad || CASE WHEN a IS DISTINCT FROM c THEN ' pushdown' ELSE '' END || CASE WHEN b IS DISTINCT FROM c THEN ' scan' ELSE '' END;
	END LOOP;
	SELECT sum(rows) INTO idxrows FROM sift.col_segments(idx::regclass);
	SELECT count(*) INTO heaprows FROM w;
	RETURN tag || ': ' || CASE WHEN bad = '' THEN 'agree' ELSE 'DIFFER:' || bad END || ', index lists ' || CASE WHEN idxrows = heaprows THEN 'all' WHEN idxrows > heaprows AND NOT exact THEN 'all' WHEN idxrows > heaprows THEN 'too many' ELSE 'too few' END || ' rows, ' || heaprows || ' rows';
END $$;

-- statements of one row, then of many
BEGIN;
INSERT INTO w VALUES (1001, 1, 1, 'one');
SELECT agree('one row');
INSERT INTO w VALUES (1002, 2, 2, 'two');
INSERT INTO w VALUES (1003, 3, 3, 'three');
SELECT agree('three rows, one by one');
INSERT INTO w SELECT 2000 + g, g % 7, g % 11, 'm' || g % 3 FROM generate_series(1, 700) g;
SELECT agree('a statement of 700 rows (several pages)');
COMMIT;
SELECT agree('after commit');

-- a row wider than a page goes in with the rows around it, in order
BEGIN;
INSERT INTO w VALUES (3001, 1, 1, 'before');
INSERT INTO w VALUES (3002, 2, 2, repeat(md5('wide'), 400));
INSERT INTO w SELECT 3100 + g, g % 7, g, repeat(md5(g::text), 130) FROM generate_series(1, 12) g;
INSERT INTO w VALUES (3003, 3, 3, 'after');
SELECT agree('wide rows');
ROLLBACK;
SELECT agree('after rollback');

-- COPY, then a statement that reads, then more rows
BEGIN;
COPY w FROM STDIN WITH (FORMAT csv);
4001,1,10.5,c1
4002,2,11.5,c2
4003,3,12.5,c3
4004,4,13.5,c4
4005,5,14.5,c5
\.
SELECT agree('copy');
COPY w FROM STDIN WITH (FORMAT csv);
4006,6,15.5,c6
4007,0,16.5,c7
\.
INSERT INTO w VALUES (4008, 1, 1, 'c8');
SELECT agree('copy, a read, copy and an insert');
COMMIT;

-- new versions of rows (an update of an indexed column is a new row of the index), deletes
BEGIN;
INSERT INTO w SELECT 5000 + g, g % 7, g, 'u' FROM generate_series(1, 40) g;
UPDATE w SET k = (k + 1) % 7, v = v + 1 WHERE id % 5 = 0 AND id < 5100;
DELETE FROM w WHERE id BETWEEN 5010 AND 5020;
SELECT agree('inserts, updates and deletes');
COMMIT;
SELECT agree('after commit');

-- savepoints: the rows of the part that rolled back are not there, the rest is
BEGIN;
INSERT INTO w SELECT 6000 + g, g % 7, g, 's' FROM generate_series(1, 30) g;
SAVEPOINT a;
INSERT INTO w SELECT 6100 + g, g % 7, g, 's' FROM generate_series(1, 30) g;
SAVEPOINT b;
INSERT INTO w SELECT 6200 + g, g % 7, g, 's' FROM generate_series(1, 30) g;
RELEASE b;
SELECT agree('inside two savepoints');
ROLLBACK TO a;
SELECT agree('after ROLLBACK TO a');
INSERT INTO w SELECT 6300 + g, g % 7, g, 's' FROM generate_series(1, 30) g;
COMMIT;
SELECT agree('after commit') AS savepoints, (SELECT count(*) FROM w WHERE id BETWEEN 6100 AND 6299) AS from_rolled_back_part;

-- an error in a PL/pgSQL block rolls back its rows alone
DO $$
BEGIN
	INSERT INTO w SELECT 7000 + g, g % 7, g, 'e' FROM generate_series(1, 20) g;
	BEGIN
		INSERT INTO w SELECT 7100 + g, g % 7, g, 'e' FROM generate_series(1, 20) g;
		INSERT INTO w VALUES (7000, 0, 0, 'duplicate key');
	EXCEPTION WHEN unique_violation THEN NULL;
	END;
	INSERT INTO w SELECT 7200 + g, g % 7, g, 'e' FROM generate_series(1, 20) g;
END $$;
SELECT agree('block error') AS block_error, (SELECT count(*) FROM w WHERE id BETWEEN 7100 AND 7199) AS from_failed_block;

-- an error of the statement that holds rows rolls the transaction back, and the next transaction starts clean
BEGIN;
INSERT INTO w SELECT 8000 + g, g % 7, g, 'f' FROM generate_series(1, 20) g;
INSERT INTO w VALUES (8000, 0, 0, 'duplicate key');
ROLLBACK;
SELECT agree('after the failed transaction'), (SELECT count(*) FROM w WHERE id >= 8000) AS rows_of_it;

-- ON CONFLICT: rows that lose leave no index entry
BEGIN;
INSERT INTO w SELECT 9000 + g, g % 7, g, 'o' FROM generate_series(1, 20) g;
INSERT INTO w SELECT g, 1, 1, 'conflict' FROM generate_series(250, 350) g ON CONFLICT (id) DO NOTHING;
INSERT INTO w SELECT 9100 + g, g % 7, g, 'o' FROM generate_series(1, 20) g;
INSERT INTO w SELECT g, 2, 2, 'upsert' FROM generate_series(280, 290) g ON CONFLICT (id) DO UPDATE SET k = excluded.k, v = excluded.v;
SELECT agree('on conflict');
COMMIT;
SELECT agree('after commit');

-- the catalog functions of the index read the rows of the transaction
BEGIN;
SELECT coalesce(sum(rows), 0) AS before FROM sift.col_segments('w_col') WHERE kind <> 'immutable' \gset
INSERT INTO w SELECT 10000 + g, g % 7, g, 'i' FROM generate_series(1, 90) g;
SELECT coalesce(sum(rows), 0) - :before AS buffered_rows_seen FROM sift.col_segments('w_col') WHERE kind <> 'immutable';
SELECT sift.col_verify('w_col', true)::text = '' AS verified;
INSERT INTO w SELECT 10100 + g, g % 7, g, 'i' FROM generate_series(1, 90) g;
SELECT sift.col_promote('w_col') >= 0 AS promoted;
INSERT INTO w SELECT 10200 + g, g % 7, g, 'i' FROM generate_series(1, 90) g;
SELECT agree('promote inside the transaction');
SELECT sift.col_merge('w_col', true) >= 0 AS merged;
INSERT INTO w SELECT 10300 + g, g % 7, g, 'i' FROM generate_series(1, 20) g;
SELECT agree('merge inside the transaction');
COMMIT;
SELECT agree('after commit');

-- the index dropped, truncated, rebuilt or recreated in the transaction: the rows it was given before are not written again
BEGIN;
INSERT INTO w SELECT 11000 + g, g % 7, g, 'd' FROM generate_series(1, 50) g;
DROP INDEX w_col;
INSERT INTO w SELECT 11100 + g, g % 7, g, 'd' FROM generate_series(1, 50) g;
CREATE INDEX w_col ON w USING sift_col (id, k, v, s) WITH (write_buffer_limit = 64);
INSERT INTO w SELECT 11200 + g, g % 7, g, 'd' FROM generate_series(1, 50) g;
SELECT agree('drop and create', true);
COMMIT;
SELECT agree('after commit');

BEGIN;
INSERT INTO w SELECT 12000 + g, g % 7, g, 'r' FROM generate_series(1, 50) g;
REINDEX INDEX w_col;
INSERT INTO w SELECT 12100 + g, g % 7, g, 'r' FROM generate_series(1, 50) g;
SELECT agree('reindex', true);
COMMIT;
SELECT agree('after commit');

BEGIN;
INSERT INTO w SELECT 13000 + g, g % 7, g, 't' FROM generate_series(1, 50) g;
TRUNCATE w;
INSERT INTO w SELECT 13100 + g, g % 7, g, 't' FROM generate_series(1, 30) g;
SELECT agree('truncate', true);
COMMIT;
SELECT agree('after commit');

BEGIN;
INSERT INTO w SELECT 14000 + g, g % 7, g, 'a' FROM generate_series(1, 50) g;
ALTER TABLE w ALTER COLUMN v TYPE numeric(12,3);
INSERT INTO w SELECT 14100 + g, g % 7, g, 'a' FROM generate_series(1, 30) g;
SELECT agree('alter column type', true);
COMMIT;
SELECT agree('after commit');

BEGIN;
INSERT INTO w SELECT 15000 + g, g % 7, g, 'x' FROM generate_series(1, 50) g;
CLUSTER w USING w_pkey;
INSERT INTO w SELECT 15100 + g, g % 7, g, 'x' FROM generate_series(1, 30) g;
SELECT agree('cluster', true);
COMMIT;
SELECT agree('after commit');

BEGIN;
INSERT INTO w SELECT 16000 + g, g % 7, g, 'x' FROM generate_series(1, 50) g;
DROP TABLE w;
COMMIT;
SELECT to_regclass('w_col') IS NULL AS dropped;

-- two column indexes on one table, and a table that is written by statements of two tables
CREATE TABLE w2 (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
CREATE INDEX w2_a ON w2 USING sift_col (id, k);
CREATE INDEX w2_b ON w2 USING sift_col (id, v);
CREATE TABLE w3 (id int PRIMARY KEY, k int) WITH (autovacuum_enabled = false);
CREATE INDEX w3_a ON w3 USING sift_col (id, k);
BEGIN;
INSERT INTO w2 SELECT g, g % 5, g % 9 FROM generate_series(1, 400) g;
INSERT INTO w3 SELECT g, g % 3 FROM generate_series(1, 300) g;
INSERT INTO w2 SELECT 1000 + g, g % 5, g % 9 FROM generate_series(1, 100) g;
SELECT (SELECT count(*) FROM w2 WHERE k = 1) AS w2_k1, (SELECT count(*) FROM w2 WHERE v = 4) AS w2_v4, (SELECT count(*) FROM w3 WHERE k = 2) AS w3_k2;
SET LOCAL sift.enable_agg = off;
SELECT (SELECT count(*) FROM w2 WHERE k = 1) AS w2_k1, (SELECT count(*) FROM w2 WHERE v = 4) AS w2_v4, (SELECT count(*) FROM w3 WHERE k = 2) AS w3_k2;
COMMIT;
SELECT sift.col_verify('w2_a', true)::text = '' AS a, sift.col_verify('w2_b', true)::text = '' AS b, sift.col_verify('w3_a', true)::text = '' AS c;
SELECT (SELECT sum(rows) FROM sift.col_segments('w2_a')) AS rows_a, (SELECT sum(rows) FROM sift.col_segments('w2_b')) AS rows_b, (SELECT sum(rows) FROM sift.col_segments('w3_a')) AS rows_c;
DROP TABLE w2, w3;
DROP FUNCTION agree(text, bool, text);
DROP EXTENSION pg_sift;
