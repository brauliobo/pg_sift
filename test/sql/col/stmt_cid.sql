-- A statement that modifies data runs with one command id (the one its tuples carry), and a function that runs SQL inside it (SPI) advances the current one. The index of a
-- track_changes key ends the entries of the versions a statement replaces: its tombstones carry the command id of the statement, and the probe that finds the old versions
-- reads them with a snapshot of the statement's command (sift_col_stmt_cid, col/am/am.c). With the current command id the probe of every row after the function's first call
-- no longer saw the version the UPDATE ended, kept its entry, and a read counted the row twice. Every case runs the statements in a transaction, and the aggregate through the
-- index (heap-free and checked) gives the rows of the heap (the oracle) after each statement and after the commit.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE tk (code text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO tk SELECT 'c' || i, i % 10, i FROM generate_series(1, 20000) i;
CREATE INDEX tk_c ON tk USING sift_col (code, tenant, amount);
VACUUM (FREEZE, ANALYZE) tk;

CREATE FUNCTION f_perform() RETURNS int LANGUAGE plpgsql AS $$ BEGIN PERFORM 1; RETURN 0; END $$;
CREATE FUNCTION f_execute() RETURNS int LANGUAGE plpgsql AS $$ DECLARE r int; BEGIN EXECUTE 'SELECT count(*)::int FROM pg_class' INTO r; RETURN r - r; END $$;
CREATE FUNCTION f_definer() RETURNS int LANGUAGE plpgsql SECURITY DEFINER AS $$ BEGIN PERFORM count(*) FROM pg_class; RETURN 0; END $$;
-- an AFTER trigger of one row that updates another row of the same table: a statement of its own inside the statement
CREATE FUNCTION bump() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN UPDATE tk SET amount = amount + 1 WHERE code = 'c71'; RETURN NULL; END $$;
CREATE TRIGGER tk_bump AFTER UPDATE ON tk FOR EACH ROW WHEN (OLD.code = 'c70') EXECUTE FUNCTION bump();

-- dynamic SQL: a cached plan would keep the plan of the first call whatever the settings are
CREATE FUNCTION answer(mode int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.tk_sums_with_plan_check(mode) $$;
CREATE FUNCTION chk(what text, OUT test text, OUT free_equals_heap bool, OUT checked_equals_heap bool) LANGUAGE plpgsql AS $$
BEGIN
	test := what;
	free_equals_heap := answer(0) = answer(2);
	checked_equals_heap := answer(1) = answer(2);
END $$;

-- a function with PERFORM, one with EXECUTE and a SECURITY DEFINER one, in the target list of three UPDATEs of the same rows
BEGIN;
UPDATE tk SET amount = amount + 1 + f_perform() WHERE code IN (SELECT 'c' || i FROM generate_series(7, 20000, 7) i);
SELECT * FROM chk('perform, in the transaction');
UPDATE tk SET amount = amount + 500 + f_execute() WHERE code IN (SELECT 'c' || i FROM generate_series(21, 20000, 21) i);
SELECT * FROM chk('execute, in the transaction');
UPDATE tk SET amount = amount + 5 + f_definer() WHERE code IN (SELECT 'c' || i FROM generate_series(5, 20000, 5) i);
SELECT * FROM chk('security definer, in the transaction');
COMMIT;
SELECT * FROM chk('functions');

-- the trigger that updates another row of the table
BEGIN;
UPDATE tk SET amount = amount + 2 + f_perform() WHERE code IN ('c70', 'c71', 'c72', 'c73');
SELECT * FROM chk('trigger, in the transaction');
COMMIT;
SELECT * FROM chk('trigger');

-- an UPDATE in a CTE
BEGIN;
WITH u AS (UPDATE tk SET amount = amount + 3 + f_perform() WHERE code IN (SELECT 'c' || i FROM generate_series(3, 20000, 3) i) RETURNING code)
UPDATE tk SET amount = amount + 4 + f_perform() WHERE code IN (SELECT 'c' || i FROM generate_series(2, 20000, 2) i) AND code NOT IN (SELECT code FROM u);
SELECT * FROM chk('cte, in the transaction');
COMMIT;
SELECT * FROM chk('cte');

-- MERGE
BEGIN;
MERGE INTO tk t USING (SELECT 'c' || i AS code FROM generate_series(11, 20000, 11) i) s ON t.code = s.code
	WHEN MATCHED THEN UPDATE SET amount = t.amount + 6 + f_perform();
SELECT * FROM chk('merge, in the transaction');
UPDATE tk SET amount = amount + 7 + f_perform() WHERE code IN (SELECT 'c' || i FROM generate_series(11, 20000, 22) i);
SELECT * FROM chk('merge and update, in the transaction');
COMMIT;
SELECT * FROM chk('merge');

-- a cursor and UPDATE ... WHERE CURRENT OF
BEGIN;
DECLARE cur CURSOR FOR SELECT code FROM tk WHERE code IN (SELECT 'c' || i FROM generate_series(13, 20000, 13) i) FOR UPDATE;
MOVE FORWARD 39 IN cur;
FETCH FROM cur;
UPDATE tk SET amount = amount + 8 + f_perform() WHERE CURRENT OF cur;
FETCH FROM cur;
UPDATE tk SET amount = amount + 9 + f_perform() WHERE CURRENT OF cur;
SELECT * FROM chk('cursor, in the transaction');
CLOSE cur;
COMMIT;
SELECT * FROM chk('cursor');

-- a savepoint rolled back in the middle: the statements before and after it, and the ones it undid
BEGIN;
UPDATE tk SET amount = amount + 10 + f_perform() WHERE code IN (SELECT 'c' || i FROM generate_series(17, 20000, 17) i);
SAVEPOINT s;
UPDATE tk SET amount = amount + 11 + f_perform() WHERE code IN (SELECT 'c' || i FROM generate_series(17, 20000, 34) i);
SELECT * FROM chk('savepoint, before the rollback');
ROLLBACK TO s;
SELECT * FROM chk('savepoint, after the rollback');
UPDATE tk SET amount = amount + 12 + f_perform() WHERE code IN (SELECT 'c' || i FROM generate_series(17, 20000, 51) i);
SELECT * FROM chk('savepoint, after the next update');
COMMIT;
SELECT * FROM chk('savepoint');

DROP TRIGGER tk_bump ON tk;
DROP FUNCTION chk(text), answer(int), bump(), f_perform(), f_execute(), f_definer();
DROP TABLE tk;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
