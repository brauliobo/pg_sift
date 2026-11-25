-- Projections of a sift_table against DDL and transactions: a metadata-only ALTER TABLE (ADD, DROP or ALTER COLUMN TYPE) changes the layout of an ORDER BY projection that keeps every column, so the parts
-- it has are another projection: the answers stay the table's and the next maintenance builds it again; sift.create_projection and sift.drop_projection follow ROLLBACK, ROLLBACK TO SAVEPOINT and PREPARE
-- TRANSACTION as the catalog row of the definition does, so a rolled-back create leaves no projection and a rolled-back drop leaves it whole. Every check prints t.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE t (a int, b int, c text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, (i * 7919) % 1000, 'c' || i FROM generate_series(1, 5000) i;
VACUUM t;
SELECT sift.create_projection('t', 'by_b', 'ORDER BY b');

CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, '';'' ORDER BY r::text) FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT string_agg(r::text, '';'' ORDER BY r::text) FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;
CREATE FUNCTION uses_projection(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftProjSortScan%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION projections() RETURNS TABLE (name text, state text) LANGUAGE sql AS
$$ SELECT p.name, p.state FROM sift.projection_list() p WHERE p.relation = 't'::regclass ORDER BY p.name $$;

SELECT * FROM projections();
SELECT uses_projection('SELECT * FROM t ORDER BY b LIMIT 5') AS served;

-- ADD COLUMN: the projection parts do not hold d; the answers are the table's, maintenance builds the projection again
ALTER TABLE t ADD COLUMN d int DEFAULT 42;
INSERT INTO t SELECT 5000 + i, i % 1000, 'n' || i, i FROM generate_series(1, 200) i;
SELECT same_rows('SELECT * FROM t ORDER BY b, a LIMIT 50', 'SELECT * FROM t ORDER BY b + 0, a LIMIT 50') AS add_column_same;
VACUUM t;
SELECT same_rows('SELECT * FROM t ORDER BY b, a LIMIT 50', 'SELECT * FROM t ORDER BY b + 0, a LIMIT 50') AS add_column_vacuumed_same;
SELECT * FROM projections();
SELECT uses_projection('SELECT * FROM t ORDER BY b LIMIT 5') AS served_again;

-- DROP COLUMN of a column in the middle of the layout
ALTER TABLE t DROP COLUMN a;
INSERT INTO t SELECT i % 1000, 'm' || i, i FROM generate_series(1, 200) i;
SELECT same_rows('SELECT * FROM t ORDER BY b, c LIMIT 50', 'SELECT * FROM t ORDER BY b + 0, c LIMIT 50') AS drop_column_same;
VACUUM t;
SELECT same_rows('SELECT * FROM t ORDER BY b, c LIMIT 50', 'SELECT * FROM t ORDER BY b + 0, c LIMIT 50') AS drop_column_vacuumed_same;
SELECT * FROM projections();
SELECT uses_projection('SELECT * FROM t ORDER BY b LIMIT 5') AS served_after_drop;

-- ALTER COLUMN TYPE
ALTER TABLE t ALTER COLUMN d TYPE bigint;
VACUUM t;
SELECT same_rows('SELECT * FROM t ORDER BY b, c LIMIT 50', 'SELECT * FROM t ORDER BY b + 0, c LIMIT 50') AS alter_type_same;
SELECT * FROM projections();
SELECT uses_projection('SELECT * FROM t ORDER BY b LIMIT 5') AS served_after_type;

-- a rolled-back create leaves no projection, not even after maintenance
BEGIN;
SELECT sift.create_projection('t', 'p_rb', 'ORDER BY c');
ROLLBACK;
VACUUM t;
SELECT * FROM projections();
BEGIN;
SAVEPOINT sp;
SELECT sift.create_projection('t', 'p_sp', 'ORDER BY d');
ROLLBACK TO sp;
COMMIT;
VACUUM t;
SELECT * FROM projections();

-- a rolled-back drop leaves the projection whole and serving
BEGIN;
SELECT sift.drop_projection('t', 'by_b');
ROLLBACK;
SELECT * FROM projections();
SELECT uses_projection('SELECT * FROM t ORDER BY b LIMIT 5') AS served_after_rolled_back_drop;
BEGIN;
SAVEPOINT sp;
SELECT sift.drop_projection('t', 'by_b');
ROLLBACK TO sp;
COMMIT;
SELECT * FROM projections();
SELECT uses_projection('SELECT * FROM t ORDER BY b LIMIT 5') AS served_after_rolled_back_savepoint;

-- a drop that commits takes the projection with it at once, inside a transaction block too
BEGIN;
SELECT sift.drop_projection('t', 'by_b');
COMMIT;
SELECT * FROM projections();
SELECT count(*) AS no_projection_left FROM sift.projection_list() WHERE relation = 't'::regclass;

-- a transaction that dropped a projection cannot be prepared: nobody could roll the drop back
SELECT sift.create_projection('t', 'by_c', 'ORDER BY c');
BEGIN;
SELECT sift.drop_projection('t', 'by_c');
PREPARE TRANSACTION 'p98_proj';
ROLLBACK;
SELECT format('ROLLBACK PREPARED %L', gid) FROM pg_prepared_xacts WHERE gid = 'p98_proj' \gexec
SELECT * FROM projections();
SELECT sift.drop_projection('t', 'by_c');

DROP TABLE t;
DROP FUNCTION same_rows(text, text), uses_projection(text), projections();
DROP EXTENSION pg_sift;
