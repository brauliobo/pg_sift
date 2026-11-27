-- Deletion records of the tombstone log: a statement that deletes many rows of the units of an index writes one record for each unit that holds enough of them, and marks for the rest.
-- Every state is compared with the answer of the heap: the records of a statement in progress, aborted, rolled back to a savepoint, committed and not yet folded, folded into the liveness
-- runs, carried to marks by a merge, and dropped by VACUUM.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
-- Format 1: the tombstone log is Format 1.
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text, note text) WITH (autovacuum_enabled = false, fillfactor = 60);
INSERT INTO events SELECT i, i % 10, i, 'x', repeat('n', 150) FROM generate_series(1, 30000) i;
CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id');
VACUUM (FREEZE) events;
SELECT sum(units) > 1 AS several_units FROM sift.col_segments('events_c') WHERE kind = 'immutable';

CREATE FUNCTION chk(what text, tbl text DEFAULT 'events', OUT test text, OUT same_as_heap bool, OUT sift_plan bool) LANGUAGE plpgsql AS $$
DECLARE a text; o text; q text; ex text;
BEGIN
	test := what;
	same_as_heap := true;
	sift_plan := false;
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR ex IN EXECUTE 'EXPLAIN (COSTS OFF) SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY 1' LOOP
		sift_plan := sift_plan OR ex LIKE '%SiftAgg%';
	END LOOP;
	FOREACH q IN ARRAY ARRAY[
		'SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY 1 ORDER BY 1',
		'SELECT count(*), sum(counter_a), min(id), max(id) FROM events',
		'SELECT count(*) FROM events WHERE id BETWEEN 1000 AND 9000 AND tenant_id IN (2, 5)',
		'SELECT id FROM events WHERE id BETWEEN 5000 AND 5100 ORDER BY id']
	LOOP
		q := replace(q, ' events', ' ' || tbl);
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO a;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO o;
		same_as_heap := same_as_heap AND a IS NOT DISTINCT FROM o;
	END LOOP;
END $$;

CREATE FUNCTION log_kinds(idx text DEFAULT 'events_c', OUT marks int, OUT records int) LANGUAGE sql AS
	$$ SELECT (SELECT count(*)::int FROM sift._test_col_log_entries(idx::regclass) WHERE kind = 1 AND flags & 32768 = 0),
	          (SELECT count(*)::int FROM sift._test_col_log_records(idx::regclass)) $$;
CREATE FUNCTION dead(idx text DEFAULT 'events_c') RETURNS bigint LANGUAGE sql AS $$ SELECT dead_rows FROM sift.col_index_status WHERE index::text = idx $$;

SELECT * FROM chk('fresh');

-- a small statement writes marks only: the lookup of its rows in the units would cost more than they do
DELETE FROM events WHERE id % 5000 = 1;
SELECT (log_kinds()).marks AS marks, (log_kinds()).records AS records;
SELECT * FROM chk('few deletes, marks');

-- a large one writes records
DELETE FROM events WHERE id % 20 = 3;
SELECT (log_kinds()).records > 0 AS records_written, (log_kinds()).marks AS marks_after;
SELECT * FROM chk('scattered deletes, records');

-- a cursor opened before the statement does not see its records: the command id of a record is part of the rule
CREATE FUNCTION cursor_keeps_its_snapshot(OUT same_before_and_after bool, OUT records_in_own_transaction bool) LANGUAGE plpgsql AS $$
DECLARE
	c CURSOR FOR SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY 1 ORDER BY 1;
	before text; after text := ''; r record;
BEGIN
	SELECT string_agg(t::text, ';') INTO before FROM (SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY 1 ORDER BY 1) t;
	OPEN c;
	DELETE FROM events WHERE id % 20 = 5;
	records_in_own_transaction := (log_kinds()).records > 0;
	LOOP
		FETCH c INTO r;
		EXIT WHEN NOT FOUND;
		after := after || CASE WHEN after = '' THEN '' ELSE ';' END || r::text;
	END LOOP;
	same_before_and_after := before = after;
END $$;
SELECT * FROM cursor_keeps_its_snapshot();
SELECT * FROM chk('after the cursor');

-- a transaction in progress, one rolled back and a savepoint rolled back
BEGIN;
DELETE FROM events WHERE id % 20 = 7;
SAVEPOINT s;
DELETE FROM events WHERE id % 20 = 9;
SELECT * FROM chk('own deletes in progress');
ROLLBACK TO s;
SELECT * FROM chk('own deletes, savepoint rolled back');
COMMIT;
BEGIN;
DELETE FROM events WHERE id % 20 = 11;
ROLLBACK;
SELECT * FROM chk('committed, rolled back and savepoint deletes');

-- the fold clears the rows of the records and drops them
SELECT sift._test_col_fold_marks('events_c') > 0 AS folded;
SELECT (log_kinds()).records AS records_after_fold, dead() > 0 AS dead_rows;
SELECT * FROM chk('folded');

-- updates that change the key, then deletes again: marks (the write buffer) and records (the units) beside each other
UPDATE events SET id = id + 100000 WHERE id % 25 = 4;
DELETE FROM events WHERE id % 15 = 2;
SELECT * FROM chk('mixed marks and records');
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT * FROM chk('promoted');

-- a merge replaces the segments the records name: they are carried to marks first
DELETE FROM events WHERE id % 12 = 5;
SELECT (log_kinds()).records > 0 AS records_before_merge;
SELECT string_agg(gen::text, ',' ORDER BY gen) AS gens_before FROM sift.col_segments('events_c') WHERE kind = 'immutable' \gset
SELECT sift.col_merge('events_c', true) >= 0 AS merged;
SELECT string_agg(gen::text, ',' ORDER BY gen) <> :'gens_before' AS segments_replaced FROM sift.col_segments('events_c') WHERE kind = 'immutable';
SELECT * FROM chk('merged with records pending');
SELECT sift._test_col_fold_marks('events_c') >= 0 AS folded_after_merge;
SELECT * FROM chk('folded after the merge');

-- VACUUM drops what is cleared
DELETE FROM events WHERE id % 14 = 6;
VACUUM events;
SELECT * FROM chk('vacuumed');
SELECT (log_kinds()).records AS records_after_vacuum;
INSERT INTO events SELECT 200000 + i, i % 10, i, 'y', 'n' FROM generate_series(1, 3000) i;
SELECT * FROM chk('inserted after vacuum');
SELECT sift.col_verify('events_c', true) IS NOT NULL AS verified;

-- a key-cut index: its units are the rows of a range of a key that has nothing to do with the heap, so a range of the heap lies in every unit and the lookup of the rows of a statement walks the
-- blocks of each unit against its addresses; a range of ids deletes whole heap pages (many rows of a block in one unit) and a stride deletes one row of a page
CREATE TABLE events_k (id int PRIMARY KEY, grp int NOT NULL, counter_a int, tenant_id int, label text, note text) WITH (autovacuum_enabled = false, fillfactor = 60);
INSERT INTO events_k SELECT i, (i::bigint * 7919 % 300)::int, i, i % 10, 'x', repeat('n', 150) FROM generate_series(1, 30000) i;
CREATE INDEX events_kc ON events_k USING sift_col (id, grp, counter_a, tenant_id) WITH (key_cut = 'grp', track_changes = 'id');
VACUUM (FREEZE) events_k;
SELECT sum(units) > 1 AS several_units FROM sift.col_segments('events_kc') WHERE kind = 'immutable';
SELECT * FROM chk('key-cut, fresh', 'events_k');
DELETE FROM events_k WHERE id BETWEEN 2001 AND 9000;
SELECT (log_kinds('events_kc')).records > 0 AS records_for_the_range, (log_kinds('events_kc')).marks AS marks_for_the_range;
SELECT * FROM chk('key-cut, a range deleted', 'events_k');
DELETE FROM events_k WHERE id % 7 = 3;
SELECT * FROM chk('key-cut, a stride deleted', 'events_k');
DELETE FROM events_k WHERE id % 4999 = 2;
SELECT * FROM chk('key-cut, a few deleted', 'events_k');
SELECT sift._test_col_fold_marks('events_kc') > 0 AS folded;
SELECT (log_kinds('events_kc')).records AS records_after_fold, dead('events_kc') = 30000 - (SELECT count(*) FROM events_k) AS every_deleted_row_cleared;
SELECT * FROM chk('key-cut, folded', 'events_k');

DROP TABLE events, events_k;
DROP FUNCTION chk(text, text), log_kinds(text), dead(text), cursor_keeps_its_snapshot();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
