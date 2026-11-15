-- The steps of a sift (BM25) index run in a subtransaction (bm25/step.c, run_step), and the commit of a subtransaction advances the command id. The aminsert of the BM25 index of a
-- table calls them when its write buffer seals (promotion, merges), so an UPDATE of a table that has a BM25 index and a sift_col index with track_changes runs them inside the
-- statement, with no function of the user in it. The tombstones and the probe of the sift_col index read the command id of the statement (sift_col_stmt_cid, col_stmt_cid): the
-- aggregate through the index (heap-free and checked) gives the heap's rows (the oracle) after each statement and after the commit. Without the statement's command id every row
-- after the first step was counted twice.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE tk (code text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL, memo text NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO tk SELECT 'c' || i, i % 10, i, 'alpha beta gamma ' || i FROM generate_series(1, 20000) i;
CREATE INDEX tk_c ON tk USING sift_col (code, tenant, amount);
CREATE INDEX tk_m ON tk USING sift (memo) WITH (write_buffer_limit = 64);
VACUUM (FREEZE, ANALYZE) tk;

-- dynamic SQL: a cached plan would keep the plan of the first call whatever the settings are
CREATE FUNCTION answer(mode int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.tk_sums_with_plan_check(mode) $$;
CREATE FUNCTION chk(what text, OUT test text, OUT free_equals_heap bool, OUT checked_equals_heap bool) LANGUAGE plpgsql AS $$
BEGIN
	test := what;
	free_equals_heap := answer(0) = answer(2);
	checked_equals_heap := answer(1) = answer(2);
END $$;

-- the bm25 index starts with one segment; the statements grow it by sealing and promoting its write buffer
SELECT count(*) AS segments_before FROM sift.segments('tk_m') \gset
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
BEGIN;
UPDATE tk SET amount = amount + 1, memo = memo || repeat(' xyzzy', 20) WHERE code IN (SELECT 'c' || i FROM generate_series(7, 20000, 7) i);
SELECT * FROM chk('first update, in the transaction');
UPDATE tk SET amount = amount + 500, memo = memo || repeat(' wumpus', 20) WHERE code IN (SELECT 'c' || i FROM generate_series(21, 20000, 21) i);
SELECT * FROM chk('second update, in the transaction');
UPDATE tk SET amount = amount + 5 WHERE code IN (SELECT 'c' || i FROM generate_series(5, 20000, 5) i);
SELECT * FROM chk('third update, in the transaction');
COMMIT;
SELECT * FROM chk('committed');
SELECT count(*) > :segments_before AS bm25_steps_ran FROM sift.segments('tk_m');

DROP FUNCTION chk(text), answer(int);
DROP TABLE tk;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
