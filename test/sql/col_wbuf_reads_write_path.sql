-- The table of the reads of the write buffers (col/wbuf/reads.c) lives in the DSM registry, which a backend attaches in a subtransaction, and a subtransaction's commit advances the
-- command id. It was attached by the first backend that decoded the buffer, which can be the supersede of an UPDATE (aminsert reads the buffer): the id moved between two statements of
-- one transaction and a row updated twice kept its middle version in the index (the heap-free read counted it, the heap did not). The table is attached at planning and in the
-- worker now, never under a statement that writes. Every case runs the same aggregates through the index, with the heap-free read and with the heap checked, and through the heap
-- (the oracle): in a fresh backend that writes first, across a promotion between statements, and with workers.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

CREATE TABLE ww (id int PRIMARY KEY, k int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO ww SELECT g, g % 50, g FROM generate_series(1, 20000) g;
CREATE INDEX ww_idx ON ww USING sift_col (id, k, amount) WITH (track_changes = 'id');
VACUUM (FREEZE, ANALYZE) ww;
-- rows in the write buffer, for the supersede of the updates to read
INSERT INTO ww SELECT g, g % 50, g FROM generate_series(20001, 24000) g;

-- dynamic SQL: a cached plan would keep the plan of the first call whatever the settings are
CREATE FUNCTION ww_answer(q text, mode int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', (mode < 2)::text, true);
	PERFORM set_config('sift.test_no_heapfree', (mode = 1)::text, true);
	EXECUTE 'SELECT coalesce(string_agg(format(''%s:%s:%s'', k, n, s), '' '' ORDER BY k), ''none'') FROM (' || q || ') q' INTO r;
	RETURN r;
END $$;
-- one row per case: the heap-free read and the checked read give the heap's rows for the whole table, a set of keys and one key
CREATE FUNCTION ww_chk(what text, OUT test text, OUT whole_free bool, OUT whole_checked bool, OUT a_set bool, OUT one_key bool) LANGUAGE plpgsql AS $$
DECLARE
	qa text := 'SELECT k, count(*) AS n, sum(amount) AS s FROM ww GROUP BY k';
	qs text := 'SELECT k, count(*) AS n, sum(amount) AS s FROM ww WHERE k BETWEEN 5 AND 14 GROUP BY k';
	qo text := 'SELECT k, count(*) AS n, sum(amount) AS s FROM ww WHERE k = 7 GROUP BY k';
BEGIN
	test := what;
	whole_free := ww_answer(qa, 0) = ww_answer(qa, 2);
	whole_checked := ww_answer(qa, 1) = ww_answer(qa, 2);
	a_set := ww_answer(qs, 0) = ww_answer(qs, 2);
	one_key := ww_answer(qo, 0) = ww_answer(qo, 2);
END $$;

-- a fresh backend, whose first statements write: a row updated by two statements of one transaction, one that changes its key, and a deleted one
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
DO $$
BEGIN
	UPDATE ww SET amount = amount + 1 WHERE id % 7 = 0;
	UPDATE ww SET amount = amount WHERE id % 3 = 0;
	UPDATE ww SET k = (k + 1) % 50 WHERE id % 11 = 0;
	UPDATE ww SET amount = amount - 400 WHERE id % 13 = 0 AND amount > 600;
	UPDATE ww SET amount = amount + 500 WHERE id % 17 = 0 AND amount <= 500;
	DELETE FROM ww WHERE id % 19 = 0;
END $$;
SELECT * FROM ww_chk('updated twice in one transaction');

-- the same in the transaction that reads: the statements see their own rows
BEGIN;
UPDATE ww SET amount = amount + 3 WHERE id % 11 = 0;
UPDATE ww SET amount = amount + 4 WHERE id % 33 = 0;
SELECT * FROM ww_chk('own updates');
ROLLBACK;
SELECT * FROM ww_chk('rolled back');

-- a promotion between two statements, and writes after it
SELECT sift.col_promote('ww_idx') >= 0 AS promoted;
SELECT * FROM ww_chk('promoted');
UPDATE ww SET amount = amount + 2 WHERE id % 7 = 0;
UPDATE ww SET amount = amount + 600 WHERE id % 21 = 0;
SELECT * FROM ww_chk('updated after the promotion');

-- a promotion by the worker of another backend, between the statements of this one
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
INSERT INTO ww SELECT g, g % 50, g FROM generate_series(30001, 34000) g;
UPDATE ww SET amount = amount + 1 WHERE id BETWEEN 30001 AND 31000;
UPDATE ww SET amount = amount + 1 WHERE id BETWEEN 30501 AND 31500;
SELECT * FROM ww_chk('buffer, updated twice');
SELECT sift.col_promote('ww_idx') >= 0 AS promoted;
SELECT * FROM ww_chk('buffer promoted');

-- with workers
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT * FROM ww_chk('workers');
UPDATE ww SET amount = amount + 5 WHERE id % 9 = 0;
UPDATE ww SET amount = amount + 6 WHERE id % 27 = 0;
SELECT * FROM ww_chk('workers, updated twice');

DROP FUNCTION ww_chk(text), ww_answer(text, int);
DROP TABLE ww;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
