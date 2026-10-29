-- The sift_col index under writes: inserts land in write buffers, promotion and merge turn them into immutable ones, VACUUM clears the
-- liveness bits, aborted work and HOT chains stay invisible or consistent. Every stage is compared with the heap and verified.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;

CREATE FUNCTION col_cmp(idx regclass, preds text, heapwhere text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE tbl regclass; a bigint; b bigint; ex bigint; mi bigint;
BEGIN
	SELECT indrelid::regclass INTO tbl FROM pg_index WHERE indexrelid = idx;
	SELECT count(*) INTO a FROM sift._test_col_tids(idx, preds);
	EXECUTE format('SELECT count(*) FROM %s WHERE %s', tbl, heapwhere) INTO b;
	EXECUTE format('SELECT count(*) FROM (SELECT tid FROM sift._test_col_tids(%L, %L) EXCEPT SELECT ctid FROM %s WHERE %s) z', idx, preds, tbl, heapwhere) INTO ex;
	EXECUTE format('SELECT count(*) FROM (SELECT ctid FROM %s WHERE %s EXCEPT SELECT tid FROM sift._test_col_tids(%L, %L)) z', tbl, heapwhere, idx, preds) INTO mi;
	RETURN CASE WHEN a = b AND ex = 0 AND mi = 0 THEN 'ok' ELSE format('MISMATCH index=%s heap=%s extra=%s missing=%s', a, b, ex, mi) END;
END $$;

-- all the checks of one stage: one row of dots, or the mismatches
CREATE FUNCTION stage(idx regclass) RETURNS text LANGUAGE sql AS $$
	SELECT string_agg(CASE WHEN r = 'ok' THEN '.' ELSE r END, ' ' ORDER BY n) FROM (VALUES
		(1, col_cmp(idx, '', 'true')),
		(2, col_cmp(idx, '0:eq:7', 'tenant_id = 7')),
		(3, col_cmp(idx, '0:lt:0;2:ge:100', 'tenant_id < 0 AND qty >= 100')),
		(4, col_cmp(idx, '1:eq:paid', 'status = ''paid''')),
		(5, col_cmp(idx, '1:null', 'status IS NULL')),
		(6, col_cmp(idx, '2:gt:1000', 'qty > 1000')),
		(7, col_cmp(idx, '3:eq:1', 'express'))) c(n, r)
$$;
CREATE FUNCTION sums(idx regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a numeric; b numeric;
BEGIN
	SELECT sum(val::bigint) INTO a FROM sift._test_col_read(idx, 2, '');
	SELECT sum(qty) INTO b FROM orders;
	RETURN a || ' ' || b;
END $$;
CREATE FUNCTION kinds() RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	SELECT string_agg(kind || ':' || n, ' ' ORDER BY kind) INTO r FROM (SELECT CASE kind WHEN 'immutable' THEN 'immutable' ELSE 'buffer' END AS kind, count(*) AS n
	FROM sift.col_segments('o_col') GROUP BY 1) s;
	RETURN r;
END $$;

CREATE TABLE orders (id serial PRIMARY KEY, tenant_id int, status text, qty bigint, express bool, note text);
INSERT INTO orders (tenant_id, status, qty, express)
SELECT (i * 7919) % 1000 - 500, (ARRAY['new', 'paid', 'shipped'])[1 + i % 3], (i::bigint * 104729) % 100000, i % 2 = 0 FROM generate_series(1, 15000) i;
UPDATE orders SET status = NULL WHERE id % 17 = 0;
CREATE INDEX o_col ON orders USING sift_col (tenant_id, status, qty, express) WITH (write_buffer_limit = 64, dead_rewrite_pct = 10);
SELECT 'build', stage('o_col');
SELECT kinds();

-- inserts go to the write buffer, which is read together with the immutable ones
INSERT INTO orders (tenant_id, status, qty, express) SELECT i % 100, 'paid', i, true FROM generate_series(1, 5000) i;
SELECT 'insert', stage('o_col'), kinds();
SELECT sift.col_verify('o_col', true);

-- updates change the indexed columns, so every new version is indexed again; the old ones are dead
UPDATE orders SET tenant_id = coalesce(tenant_id, 0) + 1 WHERE id % 5 = 0;
SELECT 'update', stage('o_col');
DELETE FROM orders WHERE id % 7 = 0;
SELECT 'delete', stage('o_col');

-- aborted work is never visible, in the transaction or after it
BEGIN;
INSERT INTO orders (tenant_id, status, qty, express) SELECT 7, 'paid', 5, true FROM generate_series(1, 300);
SELECT 'in txn', stage('o_col');
ROLLBACK;
SELECT 'rollback', stage('o_col');
BEGIN;
INSERT INTO orders (tenant_id, status, qty, express) SELECT 7, 'paid', 5, true FROM generate_series(1, 10);
SAVEPOINT s;
INSERT INTO orders (tenant_id, status, qty, express) SELECT 7, 'paid', 5, true FROM generate_series(1, 20);
ROLLBACK TO s;
SELECT 'savepoint', stage('o_col');
COMMIT;
SELECT 'commit', stage('o_col');
BEGIN;
UPDATE orders SET tenant_id = 7 WHERE id % 11 = 0;
ROLLBACK;
SELECT 'aborted update', stage('o_col');
SELECT sift.col_verify('o_col', true);

-- This fixture fits the VACUUM merge allowance: dead rows are removed from the resulting immutable segments.
VACUUM orders;
SELECT 'vacuum', stage('o_col');
SELECT sift.col_verify('o_col', true);
SELECT bool_and(NOT dirty AND dead_rows = 0) AS dead_rows_reclaimed FROM sift.col_segments('o_col') WHERE kind = 'immutable';

-- promotion
SELECT sift.col_promote('o_col') >= 0 AS promoted;
SELECT 'promote', stage('o_col'), kinds();
SELECT sift.col_verify('o_col', true);

-- a new batch in a fresh write buffer, then a merge of everything into one segment without the dead rows
INSERT INTO orders (tenant_id, status, qty, express) SELECT i % 50, 'new', i, false FROM generate_series(1, 5000) i;
SELECT 'insert 2', stage('o_col');
SELECT sift.col_promote('o_col') >= 0 AS promoted;
SELECT sift.col_merge('o_col', true) >= 0 AS merged;
SELECT 'merge', stage('o_col');
SELECT count(*) FILTER (WHERE kind = 'immutable') AS immutable, sum(rows) FILTER (WHERE kind = 'immutable') = (SELECT count(*) FROM orders) AS no_dead_rows,
	   bool_or(dirty) FILTER (WHERE kind = 'immutable') AS dirty
FROM sift.col_segments('o_col');
SELECT sift.col_verify('o_col', true);

-- a write buffer limit this small seals the active segment while inserting
INSERT INTO orders (tenant_id, status, qty, express) SELECT i % 70, 'shipped', i, true FROM generate_series(1, 20000) i;
SELECT 'bulk insert', stage('o_col');
SELECT sift.col_verify('o_col', true);
SELECT count(*) > 1 AS several_segments FROM sift.col_segments('o_col');
VACUUM orders;
SELECT 'vacuum 2', stage('o_col');
SELECT sift.col_merge('o_col', true) >= 0 AS merged;
SELECT sift.col_merge('o_col') >= 0 AS merged_again;
SELECT 'merge 2', stage('o_col');
SELECT sift.col_verify('o_col', true);

-- TRUNCATE and REINDEX start over
TRUNCATE orders;
SELECT 'truncate', stage('o_col');
INSERT INTO orders (tenant_id, status, qty, express) SELECT i, 'paid', i, true FROM generate_series(1, 1000) i;
REINDEX INDEX o_col;
SELECT 'reindex', stage('o_col');
SELECT sift.col_verify('o_col', true);

-- HOT updates leave the indexed columns alone: the index row is the root of the chain (a redirect after pruning), so compare counts and values
CREATE TABLE hot (id serial PRIMARY KEY, tenant_id int, qty bigint, note text) WITH (fillfactor = 50);
INSERT INTO hot (tenant_id, qty) SELECT i % 20, i FROM generate_series(1, 20000) i;
CREATE INDEX hot_col ON hot USING sift_col (tenant_id, qty);
UPDATE hot SET note = 'changed' WHERE id % 3 = 0;
UPDATE hot SET tenant_id = tenant_id WHERE id % 4 = 0;
CREATE FUNCTION hot_state() RETURNS text LANGUAGE sql AS $$
	SELECT (SELECT count(*) FROM sift._test_col_tids('hot_col', '')) || ' ' || (SELECT count(*) FROM hot) || ' ' ||
		   (SELECT count(*) FROM sift._test_col_tids('hot_col', '0:eq:3;1:lt:5000')) || ' ' || (SELECT count(*) FROM hot WHERE tenant_id = 3 AND qty < 5000) || ' ' ||
		   (SELECT sum(val::bigint) FROM sift._test_col_read('hot_col', 1, '')) || ' ' || (SELECT sum(qty) FROM hot)
$$;
SELECT hot_state();
VACUUM hot;
SELECT hot_state();
INSERT INTO hot (tenant_id, qty) SELECT i % 20, i FROM generate_series(1, 3000) i;
UPDATE hot SET note = 'again' WHERE id % 2 = 0;
DELETE FROM hot WHERE id % 9 = 0;
SELECT hot_state();
VACUUM hot;
SELECT hot_state();
SELECT sift.col_verify('hot_col', true);
DROP TABLE hot;
DROP FUNCTION hot_state();

-- an index on an unlogged table is empty after the init fork is restored; a temporary table's index works like any other
CREATE TEMP TABLE tmp_orders (id serial, tenant_id int, qty bigint);
INSERT INTO tmp_orders (tenant_id, qty) SELECT i % 10, i FROM generate_series(1, 3000) i;
CREATE INDEX tmp_col ON tmp_orders USING sift_col (tenant_id, qty);
INSERT INTO tmp_orders (tenant_id, qty) SELECT i % 10, i FROM generate_series(1, 3000) i;
SELECT col_cmp('tmp_col', '0:eq:3;1:lt:1000', 'tenant_id = 3 AND qty < 1000');
SELECT sift.col_verify('tmp_col', true);

DROP TABLE tmp_orders;
DROP TABLE orders;
DROP FUNCTION col_cmp(regclass, text, text);
DROP FUNCTION stage(regclass);
DROP FUNCTION sums(regclass);
DROP FUNCTION kinds();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
