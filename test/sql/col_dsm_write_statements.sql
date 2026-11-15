-- The shared tables of the backends (the dictionaries of the column indexes, their statistics, the choices of the planner) are attached through the registry of the dynamic shared memory,
-- in a subtransaction, and a subtransaction's commit advances the command id. A backend attaches in the first statement that needs a table: when that is an UPDATE of an index that
-- supersedes the entries of the old versions (track_changes on a text key, whose dictionary is read), the command id moved between two statements of one transaction and a row updated
-- twice kept its middle version in the index: the read counted it twice. A table is attached by the next statement that reads now. Every case runs in a backend of its own, whose first
-- statements write, and the aggregate through the index gives the heap's rows (the oracle) with the heap-free read and with the heap checked.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE tk (code text PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO tk SELECT 'c' || i, i % 10, i FROM generate_series(1, 20000) i;
CREATE INDEX tk_c ON tk USING sift_col (code, tenant, amount) WITH (track_changes = 'code');
VACUUM (FREEZE, ANALYZE) tk;

-- dynamic SQL: a cached plan would keep the plan of the first call whatever the settings are
CREATE FUNCTION tk_answer(mode int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', (mode < 2)::text, true);
	PERFORM set_config('sift.test_no_heapfree', (mode = 1)::text, true);
	SELECT coalesce(string_agg(format('%s:%s:%s', tenant, n, s), ' ' ORDER BY tenant), 'none') INTO r
	FROM (SELECT tenant, count(*) AS n, sum(amount) AS s FROM tk GROUP BY tenant) q;
	RETURN r;
END $$;
CREATE FUNCTION tk_chk(what text, OUT test text, OUT free_equals_heap bool, OUT checked_equals_heap bool) LANGUAGE plpgsql AS $$
BEGIN
	test := what;
	free_equals_heap := tk_answer(0) = tk_answer(2);
	checked_equals_heap := tk_answer(1) = tk_answer(2);
END $$;

-- three UPDATEs of the same rows in one transaction
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
BEGIN;
UPDATE tk SET amount = amount + 1 WHERE code IN (SELECT 'c' || i FROM generate_series(7, 20000, 7) i);
UPDATE tk SET amount = amount + 500 WHERE code IN (SELECT 'c' || i FROM generate_series(21, 20000, 21) i);
UPDATE tk SET amount = amount + 5 WHERE code IN (SELECT 'c' || i FROM generate_series(5, 20000, 5) i);
SELECT * FROM tk_chk('three updates, in the transaction');
COMMIT;
SELECT * FROM tk_chk('three updates');

-- ON CONFLICT DO UPDATE twice over the same rows
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
BEGIN;
INSERT INTO tk SELECT code, tenant, amount + 7 FROM tk WHERE tenant = 3 ON CONFLICT (code) DO UPDATE SET amount = tk.amount + 11;
INSERT INTO tk SELECT code, tenant, amount + 7 FROM tk WHERE tenant IN (3, 4) ON CONFLICT (code) DO UPDATE SET amount = tk.amount + 13;
COMMIT;
SELECT * FROM tk_chk('on conflict do update');

-- DELETE, insert the rows again and update them
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
BEGIN;
CREATE TEMP TABLE gone AS SELECT * FROM tk WHERE tenant = 6;
DELETE FROM tk WHERE tenant = 6;
INSERT INTO tk SELECT code, tenant, amount + 3 FROM gone;
UPDATE tk SET amount = amount + 2 WHERE tenant = 6;
DELETE FROM tk WHERE tenant = 6 AND amount % 2 = 0;
INSERT INTO tk SELECT code, tenant, amount FROM gone WHERE amount % 2 = 1;
COMMIT;
SELECT * FROM tk_chk('delete and insert again');

-- COPY in a subtransaction, then updates of the rows it added
\c -
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
BEGIN;
SAVEPOINT s;
COPY tk FROM stdin;
c20001	1	10
c20002	2	20
c20003	3	30
\.
UPDATE tk SET amount = amount + 1 WHERE code IN ('c20001', 'c20002', 'c7');
UPDATE tk SET amount = amount + 2 WHERE code IN ('c20002', 'c20003', 'c7');
RELEASE s;
COMMIT;
SELECT * FROM tk_chk('copy in a subtransaction');

DROP FUNCTION tk_chk(text), tk_answer(int);
DROP TABLE tk;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
