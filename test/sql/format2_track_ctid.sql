-- track_changes of format major 2 is on by default and follows the address of the rows (ctid): any table, with or without a primary key of any type, one AFTER UPDATE OR DELETE row trigger,
-- the delete of a row visible to scans at once, an UPDATE that no index column sees writing nothing, the rows of a transaction that ends them again never written, and a TRUNCATE leaving no
-- row of its transaction behind.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
CREATE EXTENSION pg_walinspect;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION idx_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON) ' || q INTO j;
	RETURN (SELECT coalesce(sum(("n"->>'Actual Rows')::numeric * ("n"->>'Actual Loops')::numeric), -1)
			FROM jsonb_path_query(j, 'strict $.** ? (@."Node Type" == "Bitmap Index Scan")') n);
END $$;

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

CREATE FUNCTION triggers(t regclass) RETURNS int LANGUAGE sql AS $$
	SELECT count(*)::int FROM pg_trigger WHERE tgrelid = t AND tgname LIKE 'sift_track_%' $$;

-- What the index wrote since an LSN: the records of the generic resource manager are the index's
CREATE FUNCTION index_records(since pg_lsn) RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM pg_get_wal_records_info(since, pg_current_wal_insert_lsn()) WHERE resource_manager = 'Generic' $$;

-- One trigger, for UPDATE and DELETE of each row, on a table without a primary key, with a composite one, a uuid one and a text one, with no option said
CREATE TABLE nopk (id int, g int, v int, pad int);
CREATE TABLE cpk (a int, b int, g int, v int, PRIMARY KEY (a, b));
CREATE TABLE upk (id uuid PRIMARY KEY DEFAULT gen_random_uuid(), n int, v int);
CREATE TABLE tpk (name text PRIMARY KEY, n int, v int);
INSERT INTO nopk SELECT i, i % 7, i, 0 FROM generate_series(1, 30000) i;
INSERT INTO cpk SELECT i % 100, i, i % 7, i FROM generate_series(1, 30000) i;
INSERT INTO upk SELECT gen_random_uuid(), i, i FROM generate_series(1, 30000) i;
INSERT INTO tpk SELECT 'k' || i, i, i FROM generate_series(1, 30000) i;
CREATE INDEX nopk_i ON nopk USING sift_col (id, g, v) WITH (key_order = 'id');
CREATE INDEX cpk_i ON cpk USING sift_col (b, g, v) WITH (key_order = 'b');
CREATE INDEX upk_i ON upk USING sift_col (n, id, v) WITH (key_order = 'n');
CREATE INDEX tpk_i ON tpk USING sift_col (n, name, v) WITH (key_order = 'n');
SELECT triggers('nopk') AS nopk, triggers('cpk') AS cpk, triggers('upk') AS upk, triggers('tpk') AS tpk;
SELECT t.tgtype & 1 <> 0 AS row_level, t.tgtype & 8 <> 0 AS on_delete, t.tgtype & 16 <> 0 AS on_update, t.tgtype & 4 = 0 AS not_insert
FROM pg_trigger t WHERE t.tgrelid = 'nopk'::regclass AND t.tgname LIKE 'sift_track_%';

-- A delete is seen by the scans of the index at once, before any VACUUM, whatever the key of the table
DELETE FROM nopk WHERE id BETWEEN 100 AND 109;
DELETE FROM cpk WHERE b BETWEEN 100 AND 109;
DELETE FROM upk WHERE n BETWEEN 100 AND 109;
DELETE FROM tpk WHERE n BETWEEN 100 AND 109;
SELECT idx_rows('SELECT * FROM nopk WHERE id BETWEEN 90 AND 120') AS nopk,
	   idx_rows('SELECT * FROM cpk WHERE b BETWEEN 90 AND 120') AS cpk,
	   idx_rows('SELECT * FROM upk WHERE n BETWEEN 90 AND 120') AS upk,
	   idx_rows('SELECT * FROM tpk WHERE n BETWEEN 90 AND 120') AS tpk;
SELECT sum(total_dead) AS dead, bool_or(has_pending) AS pending FROM sift._test_f2_partitions('nopk_i');
SELECT f2_same('SELECT * FROM nopk WHERE id BETWEEN 90 AND 120') AS nopk, f2_same('SELECT n, v FROM upk WHERE n BETWEEN 90 AND 120') AS upk;

-- track_changes = off makes no trigger
CREATE TABLE untracked (id int, v int);
INSERT INTO untracked SELECT i, i FROM generate_series(1, 1000) i;
CREATE INDEX untracked_i ON untracked USING sift_col (id, v) WITH (key_order = 'id', track_changes = 'off');
SELECT triggers('untracked') AS triggers_off;
DELETE FROM untracked WHERE id <= 10;
SELECT f2_same('SELECT * FROM untracked WHERE id BETWEEN 1 AND 30') AS reads_check_the_heap;

-- An UPDATE that no index column sees (a HOT update) writes nothing to the index; one that an index column sees is written in one record of the index
CREATE TABLE hot (id int, g int, v int, pad int) WITH (fillfactor = 50);
INSERT INTO hot SELECT i, i % 7, i, 0 FROM generate_series(1, 30000) i;
CREATE INDEX hot_i ON hot USING sift_col (id, g, v) WITH (key_order = 'id');
CHECKPOINT;
SELECT pg_current_wal_insert_lsn() AS l0 \gset
UPDATE hot SET pad = pad + 1 WHERE id BETWEEN 10 AND 19;
SELECT index_records(:'l0') AS hot_update_index_records;
UPDATE hot SET v = v + 1000000 WHERE id = 1000;
SELECT pg_current_wal_insert_lsn() AS l1 \gset
UPDATE hot SET v = v + 1000000 WHERE id = 2000;
SELECT index_records(:'l1') AS warm_update_index_records;
SELECT idx_rows('SELECT * FROM hot WHERE id IN (1000, 2000)') AS updated_rows, count(*) AS heap FROM hot WHERE id IN (1000, 2000);
SELECT f2_same('SELECT * FROM hot WHERE id BETWEEN 1 AND 3000');
-- the first update of a partition (no slab, no pending deletes yet) makes the pages of both in one record
CREATE TABLE cold (id int, g int, v int) WITH (fillfactor = 50);
INSERT INTO cold SELECT i, i % 7, i FROM generate_series(1, 30000) i;
CREATE INDEX cold_i ON cold USING sift_col (id, g, v) WITH (key_order = 'id');
SELECT has_slab, has_pending FROM sift._test_f2_partitions('cold_i');
UPDATE cold SET v = v + 1000000 WHERE id = 1000;
SELECT has_slab, has_pending FROM sift._test_f2_partitions('cold_i');
SELECT f2_same('SELECT * FROM cold WHERE id BETWEEN 1 AND 3000');

-- Rows a transaction inserts and ends again are never written: no record of the index, no dead row
CREATE TABLE churn (id int, g int, v int);
INSERT INTO churn SELECT i, i % 7, i FROM generate_series(1, 30000) i;
CREATE INDEX churn_i ON churn USING sift_col (id, g, v) WITH (key_order = 'id');
SELECT sum(total_rows) AS rows_before, sum(total_dead) AS dead_before FROM sift._test_f2_partitions('churn_i') \gset
CHECKPOINT;
SELECT pg_current_wal_insert_lsn() AS l2 \gset
BEGIN;
SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off;
INSERT INTO churn SELECT i, i % 7, i FROM generate_series(100001, 100500) i;
DELETE FROM churn WHERE id > 100000;
COMMIT;
SELECT index_records(:'l2') AS insert_delete_index_records;
SELECT sum(total_rows) = :rows_before AS no_new_rows, sum(total_dead) = :dead_before AS no_dead_rows, bool_or(has_pending) AS pending FROM sift._test_f2_partitions('churn_i');
BEGIN;
SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off;
INSERT INTO churn SELECT i, i % 7, i FROM generate_series(100001, 100500) i;
UPDATE churn SET v = v + 1 WHERE id > 100000;
UPDATE churn SET id = id + 1000 WHERE id > 100000 AND id <= 100250;
COMMIT;
SELECT idx_rows('SELECT * FROM churn WHERE id > 100000') AS surviving_rows, count(*) AS heap FROM churn WHERE id > 100000;
SELECT sum(total_dead) = :dead_before AS no_dead_rows FROM sift._test_f2_partitions('churn_i');
SELECT f2_same('SELECT * FROM churn WHERE id > 99000');
-- a delete a savepoint can still undo is kept until the commit
BEGIN;
SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off;
INSERT INTO churn SELECT i, i % 7, i FROM generate_series(200001, 200100) i;
SAVEPOINT s;
DELETE FROM churn WHERE id > 200000 AND id <= 200050;
ROLLBACK TO s;
COMMIT;
SELECT idx_rows('SELECT * FROM churn WHERE id > 200000') AS after_rollback_to, count(*) AS heap FROM churn WHERE id > 200000;
BEGIN;
SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off;
INSERT INTO churn SELECT i, i % 7, i FROM generate_series(300001, 300100) i;
SAVEPOINT s;
DELETE FROM churn WHERE id > 300000 AND id <= 300050;
RELEASE s;
COMMIT;
SELECT idx_rows('SELECT * FROM churn WHERE id > 300000') AS after_release, count(*) AS heap FROM churn WHERE id > 300000;
SELECT f2_same('SELECT * FROM churn WHERE id > 199000');

-- A bulk delete keeps one deleter for each granule it touches, not one for each row
CREATE TABLE bulk (id int, g int, v int);
INSERT INTO bulk SELECT i, i % 7, i FROM generate_series(1, 40000) i;
CREATE INDEX bulk_i ON bulk USING sift_col (id, g, v) WITH (key_order = 'id');
DELETE FROM bulk WHERE id % 4 = 0;
SELECT bytes < 10000 AS under_a_byte_a_dead_row FROM sift.index_size_breakdown('bulk_i') WHERE category = 'delvec_penddel';
SELECT idx_rows('SELECT * FROM bulk WHERE id BETWEEN 1 AND 40000') AS rows_after, count(*) AS heap FROM bulk;
SELECT f2_same('SELECT g, count(*) FROM bulk WHERE id BETWEEN 1 AND 40000 GROUP BY g');
VACUUM bulk;
SELECT f2_same('SELECT g, count(*) FROM bulk WHERE id BETWEEN 1 AND 40000 GROUP BY g');

-- INSERT; TRUNCATE; COMMIT leaves no ghost row of the transaction in the index it rebuilt
CREATE TABLE trunc (id int, g int, v int);
INSERT INTO trunc SELECT i, i % 7, i FROM generate_series(1, 30000) i;
CREATE INDEX trunc_i ON trunc USING sift_col (id, g, v) WITH (key_order = 'id');
BEGIN;
SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off;
INSERT INTO trunc SELECT i, i % 7, i FROM generate_series(40001, 40100) i;
TRUNCATE trunc;
COMMIT;
SELECT count(*) AS heap, idx_rows('SELECT * FROM trunc WHERE id >= 0') AS index_rows FROM trunc;
SELECT sift._test_f2_check_keys('trunc_i');
BEGIN;
INSERT INTO trunc SELECT i, i % 7, i FROM generate_series(1, 100) i;
TRUNCATE trunc;
INSERT INTO trunc SELECT i, i % 7, i FROM generate_series(1, 5) i;
COMMIT;
SELECT count(*) AS heap, idx_rows('SELECT * FROM trunc WHERE id >= 0') AS index_rows FROM trunc;
SELECT triggers('trunc') AS trigger_kept;
DELETE FROM trunc WHERE id = 3;
SELECT idx_rows('SELECT * FROM trunc WHERE id >= 0') AS index_rows_after_delete, count(*) AS heap FROM trunc;
SELECT f2_same('SELECT * FROM trunc WHERE id >= 0');

-- Partitions: the row trigger of each leaf hears a statement on the parent; ALTER INDEX turns it off and on
CREATE TABLE ev (id int, g int, v int) PARTITION BY RANGE (id);
CREATE TABLE ev1 PARTITION OF ev FOR VALUES FROM (1) TO (20001);
CREATE TABLE ev2 PARTITION OF ev FOR VALUES FROM (20001) TO (40001);
INSERT INTO ev SELECT i, i % 7, i FROM generate_series(1, 40000) i;
CREATE INDEX ev_i ON ev USING sift_col (id, g, v) WITH (key_order = 'id');
SELECT triggers('ev') AS parent, triggers('ev1') AS leaf1, triggers('ev2') AS leaf2;
DELETE FROM ev WHERE id IN (5, 6, 25000, 25001);
SELECT idx_rows('SELECT * FROM ev1 WHERE id BETWEEN 1 AND 10') AS ev1, idx_rows('SELECT * FROM ev2 WHERE id BETWEEN 24990 AND 25010') AS ev2;
UPDATE ev SET id = id + 20000 WHERE id BETWEEN 100 AND 109;
SELECT f2_same('SELECT * FROM ev1 WHERE id BETWEEN 1 AND 40000 AND g = 3') AS ev1, f2_same('SELECT * FROM ev2 WHERE id BETWEEN 1 AND 40000 AND g = 3') AS ev2;
SELECT indexrelid::regclass AS ev1_index FROM pg_index WHERE indrelid = 'ev1'::regclass \gset
ALTER INDEX :ev1_index SET (track_changes = 'off');
SELECT triggers('ev1') AS leaf1_off, triggers('ev2') AS leaf2_on;
ALTER INDEX :ev1_index SET (track_changes = 'auto');
SELECT triggers('ev1') AS leaf1_on;
DELETE FROM ev WHERE id BETWEEN 200 AND 209;
SELECT f2_same('SELECT * FROM ev1 WHERE id BETWEEN 1 AND 40000') AS ev1, f2_same('SELECT * FROM ev2 WHERE id BETWEEN 1 AND 40000') AS ev2;

DROP INDEX nopk_i;
SELECT triggers('nopk') AS triggers_dropped;
DROP TABLE nopk, cpk, upk, tpk, untracked, hot, cold, churn, bulk, trunc, ev;
DROP FUNCTION idx_rows(text), f2_same(text), triggers(regclass), index_records(pg_lsn);
DROP EXTENSION pg_walinspect;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
