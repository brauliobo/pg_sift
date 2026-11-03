-- Which trigger each option installs, and what a heap-free read needs. track_updates (default auto, a primary key) installs the AFTER UPDATE statement trigger and says so in a NOTICE; track_deletes adds only the
-- AFTER DELETE trigger. A read leaves the heap out only when the log is complete, which needs the DELETE trigger too (a hard DELETE writes no tombstone without it): the strict reader gives the heap's rows
-- whatever was deleted. The testing switch sift.test_trust_updates takes the UPDATE trigger for enough, as a promise of no hard DELETE would: it shows the gain on an update-only table, and with a DELETE it gives wrong
-- rows (the mutant that shows why the guard is there).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = notice;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ev (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO ev SELECT i, i % 10, i FROM generate_series(1, 20000) i;
CREATE FUNCTION triggers_of(t regclass) RETURNS text LANGUAGE sql AS
$$ SELECT coalesce(string_agg(regexp_replace(tgname, '_[0-9]+$', ''), ' ' ORDER BY tgname), 'none') FROM pg_trigger WHERE tgrelid = t AND tgname LIKE 'sift%' $$;

-- the default installs the UPDATE trigger, with a NOTICE; the delete trigger only with track_deletes, and RESET leaves the UPDATE trigger
CREATE INDEX ev_a ON ev USING sift_col (id, tenant_id, counter_a);
SELECT triggers_of('ev') AS triggers, heap_free FROM sift.col_tracking('ev_a');
ALTER INDEX ev_a SET (track_deletes = on);
SELECT triggers_of('ev') AS triggers, heap_free FROM sift.col_tracking('ev_a');
ALTER INDEX ev_a RESET (track_deletes);
SELECT triggers_of('ev') AS triggers, heap_free FROM sift.col_tracking('ev_a');
-- the lifecycle. The names are sift_track_update_<index> and sift_track_delete_<index> with the oid of the trigger itself appended (PostgreSQL does it for an internal trigger; the oid of the
-- index is the trigger's argument), so a recreated trigger is a trigger of another name. CREATE INDEX makes them (above), and REINDEX of an index that has the UPDATE trigger only keeps it
CREATE TEMP TABLE before_reindex AS SELECT tgname FROM pg_trigger WHERE tgrelid = 'ev'::regclass AND tgname LIKE 'sift%';
SELECT (SELECT count(*) FROM before_reindex) AS before_reindex, regexp_replace(tgname, '[0-9]+$', 'OID') AS name FROM before_reindex;
REINDEX INDEX ev_a;
SELECT count(*) AS kept_by_reindex FROM pg_trigger WHERE tgrelid = 'ev'::regclass AND tgname IN (SELECT tgname FROM before_reindex);
-- RESET (track_updates) is the empty option, which is 'auto' with no key chosen: the trigger goes; ALTER INDEX .. SET (track_updates = 'off') and DROP INDEX drop it too
ALTER INDEX ev_a RESET (track_updates);
SELECT triggers_of('ev') AS after_reset;
REINDEX INDEX ev_a;
SELECT triggers_of('ev') AS after_reindex_makes_them;
ALTER INDEX ev_a SET (track_updates = 'off');
SELECT triggers_of('ev') AS after_off;
ALTER INDEX ev_a RESET (track_updates);
REINDEX INDEX ev_a;
DROP INDEX ev_a;
SELECT triggers_of('ev') AS after_drop;
SET client_min_messages = warning;

-- dynamic SQL: the plan is asserted to be a SiftAgg plan or not; a cached plan would keep the plan of the first call
CREATE FUNCTION answer(t text, want_sift bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text := format('SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM %s GROUP BY tenant_id', t); plan text := ''; l text; r text;
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
CREATE FUNCTION pages(t text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE format('EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT tenant_id, count(*), sum(counter_a) FROM %s GROUP BY tenant_id', t) INTO j;
	RETURN coalesce(jsonb_path_query_first(j, 'strict $.**."Heap Pages"')::int, -1);
END $$;
-- the rows of the read under the setting of the switch against the heap's (no sift plan), and whether it read heap pages
CREATE FUNCTION chk(what text, t text, trust bool, OUT test text, OUT rows_are_the_heaps bool, OUT reads_heap bool)
LANGUAGE plpgsql AS $$
DECLARE got text; oracle text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.test_trust_updates', CASE WHEN trust THEN 'on' ELSE 'off' END, true);
	got := answer(t, true);
	reads_heap := pages(t) > 0;
	PERFORM set_config('sift.test_trust_updates', 'off', true);
	PERFORM set_config('sift.enable_agg', 'off', true);
	oracle := answer(t, false);
	test := what;
	rows_are_the_heaps := got = oracle;
END $$;

-- A alone, update-only: the log is complete for a promise of no deletes (the build with the switch on writes the baseline); the strict reader does not take it
SET sift.test_trust_updates = on;
CREATE INDEX ev_a ON ev USING sift_col (id, tenant_id, counter_a);
RESET sift.test_trust_updates;
VACUUM ev;
UPDATE ev SET counter_a = counter_a + 1 WHERE id % 10 = 3;
SELECT * FROM chk('update-only, strict reader', 'ev', false);
SELECT * FROM chk('update-only, trusting the updates', 'ev', true);

-- a hard DELETE: the strict reader gives the heap's rows (it reads the heap); trusting the updates gives wrong rows (the mutant)
DELETE FROM ev WHERE id % 10 = 4 AND id < 5000;
SELECT * FROM chk('after a DELETE, strict reader', 'ev', false);
SELECT * FROM chk('after a DELETE, trusting the updates (the mutant)', 'ev', true);
DROP INDEX ev_a;

-- both triggers: the strict reader leaves the heap out and gives the heap's rows after a DELETE too
CREATE INDEX ev_b ON ev USING sift_col (id, tenant_id, counter_a) WITH (track_deletes = on);
SELECT triggers_of('ev') AS triggers, heap_free FROM sift.col_tracking('ev_b');
-- REINDEX recreates both triggers (other oids, so other names) and writes the baseline again: the log is complete from the REINDEX
DROP TABLE before_reindex;
CREATE TEMP TABLE before_reindex AS SELECT tgname FROM pg_trigger WHERE tgrelid = 'ev'::regclass AND tgname LIKE 'sift%';
REINDEX INDEX ev_b;
SELECT (SELECT count(*) FROM pg_trigger WHERE tgrelid = 'ev'::regclass AND tgname IN (SELECT tgname FROM before_reindex)) AS kept_by_reindex,
       triggers_of('ev') AS triggers, complete, heap_free FROM sift.col_tracking('ev_b');
VACUUM ev;
UPDATE ev SET counter_a = counter_a + 1 WHERE id % 10 = 5;
DELETE FROM ev WHERE id % 10 = 6 AND id < 5000;
SELECT * FROM chk('both triggers, strict reader', 'ev', false);
DROP INDEX ev_b;
DROP TABLE ev CASCADE;
DROP FUNCTION triggers_of(regclass), answer(text, bool), pages(text), chk(text, text, bool);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
