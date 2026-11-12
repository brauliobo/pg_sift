-- The marks of the tombstone log (a delete or an update of a row version, by the transaction that did it) are folded into the liveness runs of the index when the reads
-- have paid for it: the rows whose transaction every snapshot sees as committed are cleared and their tombstones dropped, which a VACUUM does for the rows the heap removed.
-- A read then takes no page of a unit through the log. What the fold must not touch stays: a transaction in progress or aborted, the chain of a heap-only update, a row that
-- took the address of a removed one.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text, note text) WITH (autovacuum_enabled = false, fillfactor = 60);
INSERT INTO events SELECT i, i % 10, i, 'x', 'n' FROM generate_series(1, 20000) i;
CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id');
VACUUM (FREEZE) events;

-- the answer of the index and of the heap for the rows and for each tenant: equal, and how many rows a read decided by the log
CREATE FUNCTION chk(what text, q text DEFAULT 'SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY 1 ORDER BY 1', OUT test text, OUT same_as_heap bool, OUT decided_by_log int)
LANGUAGE plpgsql AS $$
DECLARE a text; o text; ex text; t text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO a;
	decided_by_log := 0;
	FOR ex IN EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF) ' || q LOOP
		t := substring(ex FROM '\((\d+) of them decided by the tombstone log');
		IF t IS NOT NULL THEN decided_by_log := decided_by_log + t::int; END IF;
	END LOOP;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO o;
	test := what;
	same_as_heap := a = o;
END $$;

-- the log's entries: of an update that moved the row, and of a delete
CREATE FUNCTION marks(OUT moved int, OUT deleted int) LANGUAGE sql AS
	$$ SELECT (count(*) FILTER (WHERE kind = 2))::int, (count(*) FILTER (WHERE kind = 1))::int FROM sift._test_col_log_entries('events_c') $$;

CREATE FUNCTION dead() RETURNS bigint LANGUAGE sql AS $$ SELECT dead_rows FROM sift.col_index_status WHERE index::text = 'events_c' $$;

SELECT * FROM chk('fresh');
SELECT sift._test_col_fold_marks('events_c') AS nothing_to_fold;

-- updates that move the row: the new versions are in the write buffer, the old ones are marks
UPDATE events SET counter_a = counter_a + 5 WHERE id % 11 = 0;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT (marks()).moved AS moved_marks, dead() AS dead_before;
SELECT * FROM chk('moved, marks in the log');
SELECT sift._test_col_fold_marks('events_c') AS folded_moved, (marks()).moved AS moved_marks_after, dead() AS dead_after;
SELECT * FROM chk('moved, folded');

-- deletes
DELETE FROM events WHERE id % 13 = 0;
SELECT (marks()).deleted AS delete_marks, dead() AS dead_before;
SELECT * FROM chk('deleted, marks in the log');
SELECT sift._test_col_fold_marks('events_c') AS folded_deleted, (marks()).deleted AS delete_marks_after, dead() AS dead_after;
SELECT * FROM chk('deleted, folded');

-- a heap-only update (a column the index does not hold, room on the page) writes no mark: the entry stays right
UPDATE events SET note = 'm' WHERE id % 17 = 0 AND id % 11 <> 0 AND id % 13 <> 0;
SELECT (marks()).moved + (marks()).deleted AS marks_after_heap_only;
SELECT * FROM chk('heap-only updates');

-- a transaction in progress, and one that rolled back, mark nothing for the fold; a savepoint rolled back neither
BEGIN;
DELETE FROM events WHERE id % 19 = 0 AND id % 11 <> 0 AND id % 13 <> 0;
SAVEPOINT s;
DELETE FROM events WHERE id % 23 = 0 AND id % 11 <> 0 AND id % 13 <> 0;
ROLLBACK TO s;
SELECT sift._test_col_fold_marks('events_c') AS folded_in_progress;
SELECT * FROM chk('own deletes in progress');
COMMIT;
BEGIN;
DELETE FROM events WHERE id % 29 = 0 AND id % 11 <> 0 AND id % 13 <> 0;
ROLLBACK;
SELECT sift._test_col_fold_marks('events_c') > 0 AS folded_after_commit;
SELECT * FROM chk('committed, rolled back and savepoint deletes');
SELECT sift._test_col_fold_marks('events_c') AS nothing_more;

-- VACUUM after the fold: the rows it finds dead are not the ones the fold cleared, the addresses it frees are taken by new rows, which the fold has no mark for
VACUUM events;
SELECT (marks()).moved + (marks()).deleted AS marks_after_vacuum;
INSERT INTO events SELECT 20000 + i, i % 10, i, 'y', 'n' FROM generate_series(1, 4000) i;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT * FROM chk('addresses taken again');
UPDATE events SET counter_a = counter_a + 1 WHERE id > 20000 AND id % 7 = 0;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT * FROM chk('new rows updated, marks in the log');
SELECT sift._test_col_fold_marks('events_c') > 0 AS folded_new_rows;
SELECT * FROM chk('new rows updated and folded');

SELECT sift.col_verify('events_c') IS NOT NULL AS verified;
DROP TABLE events;

-- an index whose entries are at the root of a heap-only chain (track_changes = 'ctid'): the mark of a version that ends the chain is at the root, and the entry of the root is the one cleared
CREATE TABLE chains (id int NOT NULL, g int NOT NULL, w int NOT NULL, pad text NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 60);
CREATE UNIQUE INDEX chains_id ON chains (id);
INSERT INTO chains SELECT i, i % 10, i, 'x' FROM generate_series(1, 20000) i;
CREATE INDEX chains_c ON chains USING sift_col (g, w) WITH (track_changes = 'ctid');
VACUUM chains;
\set Q '''SELECT g, count(*), sum(w) FROM chains GROUP BY 1 ORDER BY 1'''
UPDATE chains SET pad = 'y' WHERE id % 4 = 0;
UPDATE chains SET pad = 'z' WHERE id % 8 = 0;
SELECT sift.col_promote('chains_c') >= 0 AS promoted;
SELECT * FROM chk('heap-only chains', :Q);
DELETE FROM chains WHERE id % 8 = 0;
UPDATE chains SET w = w + 1 WHERE id % 12 = 0 AND id % 8 <> 0;
SELECT sift.col_promote('chains_c') >= 0 AS promoted;
SELECT * FROM chk('chain ends: deleted and moved', :Q);
SELECT sift._test_col_fold_marks('chains_c') > 0 AS folded_chains;
SELECT * FROM chk('chain ends folded', :Q);
VACUUM chains;
SELECT * FROM chk('chains after the vacuum', :Q);
-- a VACUUM that finds too few dead rows to vacuum the index freezes the marks and leaves them: the ones whose version the heap has taken are folded
DELETE FROM chains WHERE id IN (1, 2, 3);
VACUUM chains;
SELECT (SELECT count(*) FROM sift._test_col_log_entries('chains_c') WHERE xid::text::bigint = 2) > 0 AS frozen_marks;
SELECT sift.col_promote('chains_c') >= 0 AS promoted;
SELECT * FROM chk('frozen marks', :Q);
SELECT sift._test_col_fold_marks('chains_c') AS folded_frozen;
SELECT * FROM chk('frozen marks folded', :Q);
VACUUM chains;
SELECT * FROM chk('after the final vacuum', :Q);
SELECT sift.col_verify('chains_c') IS NOT NULL AS verified;
DROP TABLE chains;
DROP FUNCTION chk(text, text), marks(), dead();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
