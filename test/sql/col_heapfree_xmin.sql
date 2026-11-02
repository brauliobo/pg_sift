-- A heap sift_col index with track_updates and track_deletes answers without the heap for the rows whose inserters it knows committed before the snapshot: the segments say so (the xid
-- stamp), the records of the write buffer carry the xid of their transaction, and the tombstone log tells what was deleted. Every case runs the same aggregate three ways: the
-- heap-free read, the same index with sift.test_no_heapfree (the heap checks every page) and no sift plan at all (the oracle), and says whether the heap pages a read touched
-- went away. A read must give the oracle's rows in every case, whether it reads the heap or not; a mutant that ignores the tombstones or the stamps must not.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO events SELECT i, i % 10, i, 'x' FROM generate_series(1, 20000) i;
CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_updates = 'id', track_deletes = on);
VACUUM events;

-- dynamic SQL: a cached plan of a SQL function would keep the plan of the first call whatever sift.enable_agg says; the plan is asserted to be a SiftAgg plan or not
CREATE FUNCTION answer(want_sift bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text := 'SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM events GROUP BY tenant_id'; plan text := ''; l text; r text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF (plan LIKE '%SiftAgg%') <> want_sift THEN
		RAISE EXCEPTION 'the plan is % a SiftAgg plan: %', CASE WHEN want_sift THEN 'not' ELSE 'still' END, plan;
	END IF;
	EXECUTE 'SELECT coalesce(string_agg(format(''%s:%s:%s'', tenant_id, n, s), '' '' ORDER BY tenant_id), ''none'') FROM (' || q || ') q' INTO r;
	RETURN r;
END $$;

-- heap pages the plan reads, from EXPLAIN ANALYZE of the same query
CREATE FUNCTION pages() RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY tenant_id' INTO j;
	RETURN coalesce(jsonb_path_query_first(j, 'strict $.**."Heap Pages"')::int, -1);
END $$;

-- one row per case: the three answers agree, and what the heap-free read saved
CREATE FUNCTION chk(what text, OUT test text, OUT same_as_heap_check bool, OUT same_as_no_sift bool, OUT free_reads_heap bool, OUT checked_reads_heap bool)
LANGUAGE plpgsql AS $$
DECLARE free_ans text; checked text; oracle text; pf int; pc int;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.test_no_heapfree', 'off', true);
	free_ans := answer(true);
	pf := pages();
	PERFORM set_config('sift.test_no_heapfree', 'on', true);
	checked := answer(true);
	pc := pages();
	PERFORM set_config('sift.test_no_heapfree', 'off', true);
	PERFORM set_config('sift.enable_agg', 'off', true);
	oracle := answer(false);
	test := what;
	same_as_heap_check := free_ans = checked;
	same_as_no_sift := free_ans = oracle;
	free_reads_heap := pf > 0;
	checked_reads_heap := pc > 0;
END $$;

-- the segments of the build say every inserter committed before the next xid; nothing in the buffer yet
SELECT count(*) AS segments, count(stamp) AS stamped FROM sift.col_stamps('events_c');
SELECT * FROM chk('fresh build');

-- an update of a column the index does not cover (HOT) clears the visibility map of the pages and leaves the entries true; the heap checks pages, the log needs none
UPDATE events SET label = 'y' WHERE id % 7 = 0;
SELECT * FROM chk('hot update');

-- an update of a covered column: a new entry in the write buffer (the xid of the update is in its record), a tombstone for the old
UPDATE events SET counter_a = counter_a + 5 WHERE id % 11 = 0;
SELECT * FROM chk('update, rows in the buffer');
-- the buffer is promoted: the new segment says its inserters committed
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT count(*) AS segments, count(stamp) AS stamped FROM sift.col_stamps('events_c');
SELECT * FROM chk('update, promoted');

-- deletes and a change of the key
DELETE FROM events WHERE id % 13 = 0;
SELECT * FROM chk('delete');
UPDATE events SET id = id + 100000 WHERE id % 17 = 0;
SELECT * FROM chk('key change');

-- merged: the segments keep what they all said
SELECT sift.col_merge('events_c', true) >= 0 AS merged;
SELECT count(*) AS segments, count(stamp) AS stamped FROM sift.col_stamps('events_c');
SELECT * FROM chk('merged');

-- a transaction that did not commit: its records are in the buffer with its xid; a read and a promotion leave them out, the tuple is seen by no one, so they cost no heap page and no stamp
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'r' FROM generate_series(30001, 30400) i; ROLLBACK;
SELECT * FROM chk('aborted insert');
BEGIN; UPDATE events SET counter_a = counter_a + 1 WHERE id % 19 = 0; ROLLBACK;
SELECT * FROM chk('aborted update');
BEGIN; DELETE FROM events WHERE id % 23 = 0; ROLLBACK;
SELECT * FROM chk('aborted delete');
BEGIN; INSERT INTO events VALUES (31001, 1, 1, 's'); SAVEPOINT a; INSERT INTO events VALUES (31002, 2, 2, 's'); UPDATE events SET counter_a = 0 WHERE id % 29 = 0; ROLLBACK TO a; COMMIT;
SELECT * FROM chk('savepoint rolled back');
-- promoted: the segment holds none of the aborted rows and says its inserters committed
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT count(*) AS segments, count(stamp) AS stamped FROM sift.col_stamps('events_c');
SELECT * FROM chk('promoted after rollbacks');
SELECT sift.col_merge('events_c', true) >= 0 AS merged;
SELECT count(*) AS segments, count(stamp) AS stamped FROM sift.col_stamps('events_c');
SELECT * FROM chk('merged after rollbacks');
-- a rebuild reads the heap
REINDEX INDEX events_c;
SELECT count(*) AS segments, count(stamp) AS stamped FROM sift.col_stamps('events_c');
SELECT * FROM chk('reindexed');

-- the transaction sees its own changes: nothing of it is settled
BEGIN;
UPDATE events SET counter_a = counter_a + 1 WHERE id % 5 = 0;
DELETE FROM events WHERE id % 31 = 0;
INSERT INTO events SELECT i, i % 10, i, 'o' FROM generate_series(40001, 40300) i;
SELECT * FROM chk('own changes');
ROLLBACK;
SELECT * FROM chk('after the rollback');

-- an upsert may kill the tuple it inserted when its conflict shows up late, and no one is told: its rows are not stamped
INSERT INTO events SELECT i, i % 10, i, 'u' FROM generate_series(100, 220) i ON CONFLICT (id) DO UPDATE SET counter_a = events.counter_a + 7;
INSERT INTO events SELECT i, i % 10, i, 'n' FROM generate_series(200, 260) i ON CONFLICT DO NOTHING;
SELECT * FROM chk('upsert');
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT count(*) AS segments, count(stamp) AS stamped FROM sift.col_stamps('events_c');
SELECT * FROM chk('upsert, promoted');
REINDEX INDEX events_c;

-- track_deletes is off: a delete is not heard, so the log says nothing and the heap checks, with the same answers
ALTER INDEX events_c RESET (track_deletes);
DELETE FROM events WHERE id % 37 = 0;
UPDATE events SET counter_a = counter_a + 2 WHERE id % 41 = 0;
SELECT * FROM chk('track_deletes off');
-- on again: a new baseline, heap-free again
ALTER INDEX events_c SET (track_deletes = on);
SELECT * FROM chk('track_deletes on again');
DELETE FROM events WHERE id % 43 = 0;
SELECT * FROM chk('delete heard');
-- a trigger disabled while rows are deleted: the delete is not heard, and the log is not taken for complete again until it is rebuilt
ALTER TABLE events DISABLE TRIGGER ALL;
DELETE FROM events WHERE id % 47 = 0;
DO $$ DECLARE n text; BEGIN FOR n IN SELECT tgname FROM pg_trigger WHERE tgrelid = 'events'::regclass AND tgname LIKE 'sift_track%' LOOP EXECUTE format('ALTER TABLE events ENABLE ALWAYS TRIGGER %I', n); END LOOP; END $$;
SELECT heap_free FROM sift.col_tracking('events_c');
SELECT * FROM chk('trigger was disabled');
ALTER INDEX events_c RESET (track_deletes);
ALTER INDEX events_c SET (track_deletes = on);
SELECT heap_free FROM sift.col_tracking('events_c');
SELECT * FROM chk('log rebuilt');

-- VACUUM gives the addresses of the deleted rows to new rows, in the same session that read the log before
DELETE FROM events WHERE id % 53 = 0;
SELECT * FROM chk('before vacuum');
VACUUM events;
INSERT INTO events SELECT i, i % 10, i, 'v' FROM generate_series(50001, 51500) i;
SELECT * FROM chk('after vacuum, new rows at the old addresses');
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT * FROM chk('promoted');
DELETE FROM events WHERE id BETWEEN 50001 AND 50500;
SELECT * FROM chk('the new rows deleted');
VACUUM events;
INSERT INTO events SELECT i, i % 10, i, 'w' FROM generate_series(52001, 52800) i;
SELECT * FROM chk('addresses reused again');

-- a build that runs beside writers knows nothing of them: no stamp and no complete log, and the answers are still the heap's
DROP INDEX events_c;
CREATE INDEX CONCURRENTLY events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_updates = 'id', track_deletes = on);
SELECT count(*) AS segments, count(stamp) AS stamped FROM sift.col_stamps('events_c');
SELECT heap_free FROM sift.col_tracking('events_c');
DELETE FROM events WHERE id % 59 = 0;
SELECT * FROM chk('concurrent build');
REINDEX INDEX events_c;
SELECT count(*) AS segments, count(stamp) AS stamped FROM sift.col_stamps('events_c');
SELECT * FROM chk('reindexed after a concurrent build');

-- the mutant of a read that takes pages for visible whatever the tombstones say gives wrong rows (the one that takes every segment for stamped is shown by a snapshot older than the
-- commits, col_heapfree_xmin.sh)
DELETE FROM events WHERE id % 61 = 0;
SET sift.test_hf_ignore_tombs = on;
SELECT same_as_no_sift AS mutant_ignoring_tombstones_agrees FROM chk('mutant, tombstones');
RESET sift.test_hf_ignore_tombs;
SELECT same_as_no_sift AS honest_read_agrees FROM chk('honest');
DROP FUNCTION chk(text);
DROP FUNCTION pages();
DROP FUNCTION answer(bool);
DROP TABLE events;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
