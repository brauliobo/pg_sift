-- The copy that REINDEX CONCURRENTLY builds has no triggers of its own: the two triggers of the tracked index move to it at the swap (PostgreSQL moves what depends on the old index), so
-- dropping the old index drops nothing that is on the table (DROP TRIGGER takes ACCESS EXCLUSIVE). The log of the copy is not complete (writers run beside the build); a plain REINDEX
-- gives the baseline. The reads check the heap meanwhile and answer as it does.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i % 10, i FROM generate_series(1, 5000) i;
CREATE INDEX t_c ON t USING sift_col (id, g, v);
VACUUM (FREEZE, ANALYZE) t;

CREATE FUNCTION triggers_of(idx regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM pg_trigger t JOIN pg_depend d ON d.refclassid = 'pg_trigger'::regclass AND d.refobjid = t.oid AND d.classid = 'pg_class'::regclass AND d.objid = idx
	WHERE t.tgrelid = 't'::regclass AND t.tgname LIKE 'sift_track_%' $$;
CREATE FUNCTION all_triggers() RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname LIKE 'sift_track_%' $$;
CREATE FUNCTION answers() RETURNS text LANGUAGE plpgsql AS $$
DECLARE free text; chk text; heap text; q text := 'SELECT string_agg(g || '':'' || n || '':'' || s, '' '' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM t GROUP BY g) q';
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	PERFORM set_config('sift.test_no_heapfree', 'false', true);
	EXECUTE q INTO free;
	PERFORM set_config('sift.test_no_heapfree', 'true', true);
	EXECUTE q INTO chk;
	PERFORM set_config('sift.enable_agg', 'false', true);
	EXECUTE q INTO heap;
	RETURN CASE WHEN free = heap AND chk = heap THEN 'same' ELSE 'DIFFERENT' END;
END $$;

SELECT triggers_of('t_c') AS triggers_of_index, all_triggers() AS triggers_on_table, answers();
SELECT cur_flags FROM sift._test_log_chan('t_c');
SELECT string_agg(oid::text, ',' ORDER BY oid) AS trigger_oids FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname LIKE 'sift_track_%' \gset
-- the writers go on meanwhile, before and after the swap
UPDATE t SET v = v + 1 WHERE id % 7 = 0;
DELETE FROM t WHERE id % 11 = 0;
REINDEX INDEX CONCURRENTLY t_c;
SELECT relname, indisvalid FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = 't'::regclass ORDER BY 1;
-- the same two triggers, now of the new index; none was made for the copy, and none left with the old index
SELECT triggers_of('t_c') AS triggers_of_index, all_triggers() AS triggers_on_table, answers();
SELECT string_agg(oid::text, ',' ORDER BY oid) = :'trigger_oids' AS the_same_triggers FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname LIKE 'sift_track_%';
SELECT cur_flags AS flags_of_the_copy FROM sift._test_log_chan('t_c');
DELETE FROM t WHERE id % 13 = 0;
SELECT answers();
-- again: the triggers of the first swap are the ones that move
REINDEX INDEX CONCURRENTLY t_c;
SELECT triggers_of('t_c') AS triggers_of_index, all_triggers() AS triggers_on_table, answers();
-- a plain REINDEX gives the baseline and its own triggers
REINDEX INDEX t_c;
SELECT triggers_of('t_c') AS triggers_of_index, all_triggers() AS triggers_on_table, answers();
SELECT cur_flags FROM sift._test_log_chan('t_c');
-- an index that was made concurrently owns its triggers, and dropping it takes them
CREATE INDEX CONCURRENTLY t_d ON t USING sift_col (id, g, v);
SELECT triggers_of('t_d') AS triggers_of_t_d, all_triggers() AS triggers_on_table;
DROP INDEX t_d;
SELECT all_triggers() AS triggers_on_table;
DROP TABLE t;
DROP FUNCTION triggers_of(regclass), all_triggers(), answers();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
