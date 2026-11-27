-- A file truncated in place (a TRUNCATE of a table made in the transaction, ON COMMIT DELETE ROWS) begins its generations, blocks and manifest versions again, and a temporary file stamps no
-- LSN: nothing a backend keeps of the file before (the filters of the keys of a UNIQUE index, the column totals of its parts, the column deltas of a sift_table) answers for the new rows.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET sift.test_create_format1 = off;
\set VERBOSITY terse
CREATE FUNCTION pg_temp.state_of(stmt text) RETURNS text LANGUAGE plpgsql AS
$$ BEGIN EXECUTE stmt; RETURN 'ok'; EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE; END $$;
-- nine entries of one key in the registry of the transaction: the next writer of the key flushes them to the L0 slab and looks for it in the slab, by its filter
CREATE PROCEDURE pg_temp.churn(k int) LANGUAGE plpgsql AS
$$ BEGIN FOR i IN 1..8 LOOP INSERT INTO u VALUES (k); DELETE FROM u WHERE id = k; END LOOP; INSERT INTO u VALUES (k); END $$;

-- 1. a UNIQUE index of a table made in the transaction, truncated between two slabs of the same shape: the duplicate of the second is refused
BEGIN;
CREATE TEMP TABLE u (id int);
CREATE UNIQUE INDEX u_i ON u USING sift_col (id);
CALL pg_temp.churn(1);
SELECT pg_temp.state_of('INSERT INTO u VALUES (1)') AS dup_before_truncate;
TRUNCATE u;
CALL pg_temp.churn(2);
SELECT pg_temp.state_of('INSERT INTO u VALUES (2)') AS dup_after_truncate;
SELECT id, count(*) FROM u WHERE id + 0 > 0 GROUP BY id;
COMMIT;
DROP TABLE u;

-- 2. the same through ON COMMIT DELETE ROWS
CREATE TEMP TABLE u (id int) ON COMMIT DELETE ROWS;
CREATE UNIQUE INDEX u_i ON u USING sift_col (id);
BEGIN;
CALL pg_temp.churn(1);
SELECT pg_temp.state_of('INSERT INTO u VALUES (1)') AS dup_first_commit;
COMMIT;
BEGIN;
CALL pg_temp.churn(2);
SELECT pg_temp.state_of('INSERT INTO u VALUES (2)') AS dup_second_commit;
SELECT id, count(*) FROM u WHERE id + 0 > 0 GROUP BY id;
COMMIT;
DROP TABLE u;

-- 3. the totals of a part of all-visible rows (COPY FREEZE) are kept for the next statement: the part of the same generation and block after a TRUNCATE has its own
BEGIN;
SET LOCAL jit = off;
SET LOCAL cpu_operator_cost = 0.00001;
CREATE TABLE a (id int, v int8);
CREATE INDEX a_i ON a USING sift_col (id, v) WITH (preagg = off);
COPY a FROM PROGRAM 'seq 1 5000 | sed "s/$/\t1/"' FREEZE;
SELECT sift.col_maintain('a_i');
EXPLAIN (COSTS OFF) SELECT sum(v), avg(v) FROM a;
SELECT sum(v), avg(v) FROM a;
TRUNCATE a;
COPY a FROM PROGRAM 'seq 1 5000 | sed "s/$/\t2/"' FREEZE;
SELECT sift.col_maintain('a_i');
SELECT sum(v), avg(v) FROM a;
SET LOCAL sift.enable_agg = off;
SELECT sum(v), avg(v) FROM a;
ROLLBACK;

-- 4. a sift_table made in the transaction with column deltas on a part, truncated and given the same rows and other deltas: every read sees the deltas after
BEGIN;
DO $$ BEGIN
	EXECUTE format('CREATE TABLE w (id int PRIMARY KEY, amount int, %s) USING sift_table', (SELECT string_agg(format('n%s int', i), ', ') FROM generate_series(1, 30) i));
END $$;
CREATE FUNCTION pg_temp.fill() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('INSERT INTO w SELECT i, 0, %s FROM generate_series(1, 3000) i', (SELECT string_agg('i', ', ') FROM generate_series(1, 30)));
END $$;
SELECT pg_temp.fill();
SELECT busy FROM sift.table_maintain('w');
SELECT ctid AS before_ctid FROM w WHERE id = 10 \gset
UPDATE w SET amount = 111 WHERE id = 10;
SELECT ctid = :'before_ctid' AS patched_in_place, sum(amount) FROM w WHERE id = 10 GROUP BY ctid;
TRUNCATE w;
SELECT pg_temp.fill();
SELECT busy FROM sift.table_maintain('w');
UPDATE w SET amount = 222 WHERE id = 20;
SELECT id, amount FROM w WHERE amount <> 0 ORDER BY id;
SET LOCAL enable_seqscan = off;
SELECT id, amount FROM w WHERE id IN (10, 20) ORDER BY id;
ROLLBACK;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
