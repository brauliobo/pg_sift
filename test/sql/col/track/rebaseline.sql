-- The log of an index that a library before the fix wrote (flags 1: 4.0.0 to 4.1.1) may lack the tombstones of row versions that a statement with a moved command id ended, and the reads
-- that take it for complete count those versions. A VACUUM re-baselines it (a tombstone for every address no version is live for) and marks it (flags 3); until then the reads check the heap.
-- The damage is made with the mutants that write no tombstone (the effect of the bug), and the flags are set as the old library left them (sift._test_log_chan).
-- Serial and on a quiet cluster: the heap-free reads depend on the xmin horizon.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
-- 200 pages or more, so that a VACUUM of a few dead rows on a page skips the vacuuming of the indexes (under 2% of the pages)
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL, pad text NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO t SELECT i, i % 10, i, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX t_c ON t USING sift_col (id, g, v);
VACUUM (FREEZE, ANALYZE) t;
SELECT relpages >= 200 AS pages_ok FROM pg_class WHERE relname = 't';

-- the answer of the heap-free read (0), of the read that checks the heap (1) and of the heap (2), for a grouped sum, and whether the plan is the sift one
CREATE FUNCTION ans(mode int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', (mode < 2)::text, true);
	PERFORM set_config('sift.test_no_heapfree', (mode = 1)::text, true);
	-- EXECUTE: a cached plan would keep the shape of the first call whatever sift.enable_agg says
	EXECUTE 'SELECT string_agg(g || '':'' || n || '':'' || s, '' '' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM t GROUP BY g) q' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION plan_is_sift() RETURNS bool LANGUAGE plpgsql AS $$
DECLARE r text; p text := '';
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR r IN EXPLAIN (COSTS OFF) SELECT g, count(*), sum(v) FROM t GROUP BY g LOOP p := p || r; END LOOP;
	RETURN p ~ 'Sift';
END $$;
CREATE FUNCTION report() RETURNS TABLE (heapfree_right bool, checked_right bool, flags int8, heap_free bool) LANGUAGE sql AS $$
	SELECT ans(0) = ans(2), ans(1) = ans(2), (SELECT cur_flags FROM sift._test_log_chan('t_c')), (SELECT heap_free FROM sift.col_tracking('t_c')) $$;
SELECT plan_is_sift();
SELECT * FROM report();

-- statements of the library before the fix: three rows (on three pages) are updated and their old versions get no tombstone
SET sift.test_no_supersede = on;
SET sift.test_no_track = on;
UPDATE t SET v = v + 1000000 WHERE id IN (1, 7000, 15000);
RESET sift.test_no_supersede;
RESET sift.test_no_track;
-- the damage is real: the log is taken for complete (flags 3), and the heap-free read counts the old versions
SELECT * FROM report();
-- control: a VACUUM that skips the indexes (few dead rows) leaves the stale entries, and a current log goes on being wrong
VACUUM t;
SELECT * FROM report();

-- the log as the old library left it: complete as it saw it, not taken for complete by this one, so the reads check the heap and are right
SELECT cur_flags FROM sift._test_log_chan('t_c', 1);
SELECT * FROM report();
SELECT * FROM sift.col_tracking('t_c');
-- the next VACUUM re-baselines it, also when it skips the indexes: the stale entries get their tombstones
UPDATE t SET v = v + 1 WHERE id = 3;
VACUUM t;
SELECT * FROM report();
SELECT * FROM sift.col_tracking('t_c');
SELECT ans(0) = ans(2) AS same_as_heap, ans(0) = ans(1) AS same_as_checked;
-- and once: the log is current, a second VACUUM adds no baseline (its entries are the same)
SELECT count(*) AS entries FROM sift._test_col_log_entries('t_c') \gset
VACUUM t;
SELECT count(*) = :entries AS log_unchanged FROM sift._test_col_log_entries('t_c');
-- the transaction that made it complete committed
SELECT pg_xact_status(cur_base_xid::text::xid8) FROM sift._test_log_chan('t_c');

-- a re-baseline that began and aborted leaves the log current and its transaction rolled back: no read takes it for complete (the heap is checked), and the next VACUUM does it again
SET sift.test_no_supersede = on;
SET sift.test_no_track = on;
UPDATE t SET v = v + 1000000 WHERE id = 9000;
RESET sift.test_no_supersede;
RESET sift.test_no_track;
BEGIN;
SELECT pg_current_xact_id()::xid AS ax \gset
ROLLBACK;
SELECT cur_flags, cur_base_xid = :'ax'::xid AS names_aborted FROM sift._test_log_chan('t_c', 3, :'ax'::xid);
SELECT ans(0) = ans(2) AS heapfree_right, ans(1) = ans(2) AS checked_right;
UPDATE t SET v = v + 1 WHERE id = 4;
VACUUM t;
SELECT * FROM report();
SELECT pg_xact_status(cur_base_xid::text::xid8) FROM sift._test_log_chan('t_c');

-- a log made while the triggers were not there (flags 0: a concurrent build, a table that was a partition) is re-baselined as well, the triggers being the option's now
SELECT cur_flags FROM sift._test_log_chan('t_c', 0);
VACUUM t;
SELECT cur_flags FROM sift._test_log_chan('t_c');
REINDEX INDEX t_c;
SELECT * FROM report();

-- the triggers made again (pg_upgrade, a restore: CREATE INDEX makes them as the option does, and the log is the file of the old cluster) are not the ones the log names, so no read takes it for
-- complete until it is re-baselined; a version that a statement ended meanwhile (the mutants) is missing from it
SELECT cur_flags FROM sift._test_log_chan('t_c', NULL, NULL, true);
SELECT triggers, complete, heap_free FROM sift.col_tracking('t_c');
SET sift.test_no_supersede = on;
SET sift.test_no_track = on;
UPDATE t SET v = v + 1000000 WHERE id IN (2, 8000, 16000);
RESET sift.test_no_supersede;
RESET sift.test_no_track;
SELECT * FROM report();
-- the next VACUUM re-baselines it, also when it skips the indexes, and the log names the triggers as they are
VACUUM t;
SELECT * FROM report();
SELECT * FROM sift.col_tracking('t_c');
SELECT count(*) AS entries FROM sift._test_col_log_entries('t_c') \gset
VACUUM t;
SELECT count(*) = :entries AS log_unchanged FROM sift._test_col_log_entries('t_c');

-- triggers that are disabled are not the option's: the writes meanwhile are heard by no one, the reads check the heap, and no VACUUM re-baselines the log
ALTER TABLE t DISABLE TRIGGER ALL;
UPDATE t SET v = v + 1 WHERE id IN (3, 9000, 17000);
SELECT * FROM report();
VACUUM t;
SELECT * FROM report();
-- enabled again they fire for the origin only (a session with session_replication_role = replica writes unheard): not the option's either, and the answers stay the heap's; VACUUM does not
-- hold the lock that enabling them for all takes (the worker does: col_track_revouch.sh)
ALTER TABLE t ENABLE TRIGGER ALL;
UPDATE t SET v = v + 1 WHERE id IN (4, 9001, 17001);
VACUUM t;
SELECT * FROM report();
SELECT string_agg(tgenabled::text, ',') AS tgenabled FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname LIKE 'sift_track_%';
-- ALTER INDEX makes them the option's again, with a new baseline
ALTER INDEX t_c SET (track_changes = 'id');
SELECT * FROM report();

DROP TABLE t;
DROP FUNCTION ans(int), plan_is_sift(), report();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
