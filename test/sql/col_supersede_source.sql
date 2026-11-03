-- The entries a new row version replaces get a tombstone when the UPDATE writes it (track_changes). The addresses of the key come from the table's unique index (a b-tree probe), or from a scan
-- of the sift index when the table has none, and the statement trigger of track_changes does not look at the rows of an UPDATE that leaves the key alone, since the replacement has
-- covered them. The same operations on three tables, one for each way (the scan of the index, the unique index with the trigger looking at every UPDATE, the unique index with the trigger
-- skipping the UPDATEs that leave the key), must leave the same tombstones and the same answers as the heap.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION mk(t text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE %I (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)', t);
	EXECUTE format('INSERT INTO %I SELECT i, i %% 10, i, ''x'' FROM generate_series(1, 20000) i', t);
	EXECUTE format('CREATE INDEX %I ON %I USING sift_col (id, tenant_id, counter_a) WITH (track_changes = ''id'')', t || '_c', t);
END $$;

-- one list of operations, each a statement of its own (a command of its own) in one transaction, the failing ones in a subtransaction that is rolled back
CREATE FUNCTION ops(t text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE s text;
BEGIN
	FOREACH s IN ARRAY ARRAY[
		'UPDATE %1$I SET counter_a = counter_a + 1 WHERE id %% 7 = 0',                          -- a covered column, the key alone
		'UPDATE %1$I SET counter_a = counter_a + 1 WHERE id %% 7 = 0',                          -- the same rows again: the version of the last command
		'UPDATE %1$I SET label = ''y'' WHERE id %% 5 = 0',                                      -- HOT: no new entry
		'UPDATE %1$I SET id = id + 100000 WHERE id %% 11 = 0',                                  -- the key changes
		'DELETE FROM %1$I WHERE id %% 13 = 0',
		'INSERT INTO %1$I SELECT i, i %% 10, i, ''r'' FROM generate_series(13, 4000, 13) i',    -- the keys come back
		'INSERT INTO %1$I VALUES (7, 7, 7, ''u''), (8, 8, 8, ''u'') ON CONFLICT (id) DO UPDATE SET counter_a = %1$I.counter_a + 100',
		'INSERT INTO %1$I VALUES (4001, 1, 1, ''n'') ON CONFLICT (id) DO UPDATE SET counter_a = 0',
		'MERGE INTO %1$I d USING (VALUES (9), (18), (27)) v(id) ON d.id = v.id WHEN MATCHED THEN UPDATE SET counter_a = d.counter_a * 2',
		'UPDATE %1$I SET id = id + 200000, tenant_id = 3 WHERE id IN (101, 102, 103)'
	] LOOP
		EXECUTE format(s, t);
	END LOOP;
	-- an update that is rolled back, one rolled back to a savepoint, and a delete rolled back: tombstones of transactions that aborted
	BEGIN
		EXECUTE format('UPDATE %I SET counter_a = counter_a + 9 WHERE id %% 17 = 0', t);
		RAISE EXCEPTION 'undo';
	EXCEPTION WHEN raise_exception THEN NULL;
	END;
	EXECUTE format('UPDATE %I SET counter_a = counter_a + 3 WHERE id %% 19 = 0', t);
	BEGIN
		EXECUTE format('UPDATE %I SET id = id + 300000 WHERE id %% 23 = 0', t);
		EXECUTE format('DELETE FROM %I WHERE id %% 29 = 0', t);
		RAISE EXCEPTION 'undo';
	EXCEPTION WHEN raise_exception THEN NULL;
	END;
	EXECUTE format('UPDATE %I SET counter_a = counter_a + 1 WHERE id %% 17 = 0', t);
END $$;

SELECT mk('events_scan'), mk('events_both'), mk('events_skip');
VACUUM events_scan, events_both, events_skip;
SET sift.test_supersede_scan = on;
SET sift.eval_methods = '-skip_trigger_lookup';
SELECT ops('events_scan');
SET sift.test_supersede_scan = off;
SELECT ops('events_both');
RESET sift.eval_methods;
SELECT ops('events_skip');

-- the tombstones: the same addresses on all three (the transactions differ, and the trigger writes a second tombstone for an address a replacement covered in an earlier transaction)
CREATE FUNCTION log_of(t text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(DISTINCT tid::text, '' '' ORDER BY tid::text) FROM sift._test_col_log_entries(%L)', t || '_c') INTO r;
	RETURN md5(r);
END $$;
SELECT log_of('events_scan') = log_of('events_both') AS scan_equals_unique, log_of('events_both') = log_of('events_skip') AS trigger_skip_equals_every_update;
SELECT (SELECT count(*) FROM sift._test_col_log_entries('events_skip_c')) > 200 AS the_log_has_tombstones;

-- the answers: the heap-free read, the heap-checked read and no sift plan agree on every table
CREATE FUNCTION answer(t text, mode int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE plan text := ''; l text; r text; q text := format('SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM %I GROUP BY tenant_id', t);
BEGIN
	PERFORM set_config('sift.enable_agg', (mode < 2)::text, true);
	PERFORM set_config('sift.test_no_heapfree', (mode = 1)::text, true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF (plan LIKE '%SiftAgg%') <> (mode < 2) THEN
		RAISE EXCEPTION 'the plan is not the one asked for: %', plan;
	END IF;
	EXECUTE 'SELECT coalesce(string_agg(format(''%s:%s:%s'', tenant_id, n, s), '' '' ORDER BY tenant_id), ''none'') FROM (' || q || ') q' INTO r;
	RETURN r;
END $$;
SELECT t, answer(t, 0) = answer(t, 1) AS free_equals_checked, answer(t, 0) = answer(t, 2) AS free_equals_no_sift
FROM (VALUES ('events_scan'), ('events_both'), ('events_skip')) v(t) ORDER BY t;
SELECT answer('events_scan', 2) = answer('events_both', 2) AND answer('events_both', 2) = answer('events_skip', 2) AS same_rows_on_all_three;

-- the rows themselves, through the index and through the heap, for the keys that changed
CREATE FUNCTION rows_of(t text, via_sift bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_col_scan', via_sift::text, true);
	PERFORM set_config('enable_seqscan', (NOT via_sift)::text, true);
	PERFORM set_config('enable_indexscan', via_sift::text, true);
	PERFORM set_config('enable_bitmapscan', via_sift::text, true);
	EXECUTE format('SELECT md5(string_agg(id || '':'' || counter_a || '':'' || label, '','' ORDER BY id)) FROM %I WHERE tenant_id = 3 OR id > 99999', t) INTO r;
	RETURN r;
END $$;
SELECT t, rows_of(t, true) = rows_of(t, false) AS through_the_index_equals_the_heap FROM (VALUES ('events_scan'), ('events_both'), ('events_skip')) v(t) ORDER BY t;

-- an UPDATE that gives the key a new value is looked at by the trigger whatever the other rows did; so is one with a BEFORE trigger that may change it
RESET sift.eval_methods;
CREATE FUNCTION touch_key() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.id := NEW.id + 500000; RETURN NEW; END $$;
CREATE TRIGGER events_skip_before BEFORE UPDATE ON events_skip FOR EACH ROW WHEN (NEW.counter_a = -1) EXECUTE FUNCTION touch_key();
UPDATE events_skip SET counter_a = -1 WHERE id IN (1, 2, 3);
SELECT answer('events_skip', 0) = answer('events_skip', 2) AS before_trigger_moved_the_key;
SELECT count(*) AS moved FROM events_skip WHERE id > 500000;
DROP TRIGGER events_skip_before ON events_skip;

-- the same, when the heap has the rows to vacuum
VACUUM events_scan, events_both, events_skip;
SELECT t, answer(t, 0) = answer(t, 2) AS after_vacuum FROM (VALUES ('events_scan'), ('events_both'), ('events_skip')) v(t) ORDER BY t;
SELECT sift.col_verify('events_skip_c', true) IS NOT NULL AS verified;

DROP FUNCTION touch_key();
DROP FUNCTION rows_of(text, bool);
DROP FUNCTION answer(text, int);
DROP FUNCTION log_of(text);
DROP FUNCTION ops(text);
DROP FUNCTION mk(text);
DROP TABLE events_scan, events_both, events_skip;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
