-- A segment written while a transaction that inserted some of its rows had not finished (a prepared transaction here: it stays open in a single session) cannot say that every
-- inserter committed, and reads check its rows in the heap. Once those transactions have finished it says it again, and a merge does not lose it: a segment that waited
-- for transactions that all committed is stamped by the next maintenance step (a promotion, even when none is due) or by the merge that takes it in; one whose transaction rolled
-- back stays unstamped, as one that never had a stamp. A read must give the oracle's rows in every state, whatever the stamp says: the heap-free read, the same index with
-- sift.test_no_heapfree (the heap checks every page) and no sift plan at all agree, in every state below.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO events SELECT i, i % 10, i, 'x' FROM generate_series(1, 20000) i;
CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id');
VACUUM events;

CREATE FUNCTION answer(want_sift bool) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.events_sums_with_plan_check(want_sift) $$;
CREATE FUNCTION pages() RETURNS int LANGUAGE sql AS $$ SELECT sift_helpers.events_heap_pages() $$;

-- one row per state: the three answers agree, and whether the heap-free read needed the heap
CREATE FUNCTION chk(what text, OUT test text, OUT same_as_heap_check bool, OUT same_as_no_sift bool, OUT free_reads_heap bool)
LANGUAGE plpgsql AS $$
DECLARE free_ans text; checked text; oracle text; pf int;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.test_no_heapfree', 'off', true);
	free_ans := answer(true);
	pf := pages();
	PERFORM set_config('sift.test_no_heapfree', 'on', true);
	checked := answer(true);
	PERFORM set_config('sift.test_no_heapfree', 'off', true);
	PERFORM set_config('sift.enable_agg', 'off', true);
	oracle := answer(false);
	test := what;
	same_as_heap_check := free_ans = checked;
	same_as_no_sift := free_ans = oracle;
	free_reads_heap := pf > 0;
END $$;

-- the segments that say nothing of their inserters
CREATE FUNCTION unstamped() RETURNS bigint LANGUAGE sql AS $$ SELECT count(*) FROM sift.col_stamps('events_c') WHERE stamp IS NULL $$;

SELECT unstamped() AS unstamped, (SELECT count(*) FROM sift.col_stamps('events_c')) AS segments;

-- committed: promoted while the transaction was open, settled by the next promotion
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'a' FROM generate_series(100001, 100300) i; PREPARE TRANSACTION 'hf_commit';
INSERT INTO events SELECT i, i % 10, i, 'c' FROM generate_series(110001, 110200) i;
SELECT * FROM chk('committed: open, in the buffer');
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('committed: open, promoted');
COMMIT PREPARED 'hf_commit';
SELECT unstamped() AS unstamped;
SELECT * FROM chk('committed: finished, not settled');
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('committed: settled');

-- rolled back: the rows are no one's; the segment stays unstamped, and a settled read is still the oracle's
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'a' FROM generate_series(120001, 120300) i; PREPARE TRANSACTION 'hf_abort';
INSERT INTO events SELECT i, i % 10, i, 'c' FROM generate_series(130001, 130200) i;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('rolled back: open, promoted');
ROLLBACK PREPARED 'hf_abort';
SELECT * FROM chk('rolled back: finished, not settled');
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('rolled back: settled');
SELECT sift.col_merge('events_c', true) >= 0 AS merged;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('rolled back: merged');
REINDEX INDEX events_c;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('rolled back: rebuilt');

-- a savepoint released with the transaction in doubt, and one rolled back: the rows of the subtransaction that did not commit are not in the segment, the others wait for the parent
BEGIN; INSERT INTO events VALUES (140001, 1, 1, 's'); SAVEPOINT s; INSERT INTO events VALUES (140002, 2, 2, 's'); RELEASE s;
INSERT INTO events VALUES (140003, 3, 3, 's'); SAVEPOINT t; INSERT INTO events VALUES (140004, 4, 4, 's'); UPDATE events SET counter_a = 0 WHERE id % 29 = 0; ROLLBACK TO t;
INSERT INTO events VALUES (140005, 5, 5, 's'); PREPARE TRANSACTION 'hf_sub';
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('subtransactions: open, promoted');
COMMIT PREPARED 'hf_sub';
SELECT * FROM chk('subtransactions: finished, not settled');
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('subtransactions: settled');

-- two transactions in doubt: the segment waits for both, and one that commits leaves it waiting
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'p' FROM generate_series(150001, 150100) i; PREPARE TRANSACTION 'hf_one';
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'q' FROM generate_series(151001, 151100) i; PREPARE TRANSACTION 'hf_two';
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
COMMIT PREPARED 'hf_one';
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('two: one committed, one open');
COMMIT PREPARED 'hf_two';
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('two: both committed');
-- ... and one rolled back after the other committed: no stamp
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'p' FROM generate_series(152001, 152100) i; PREPARE TRANSACTION 'hf_three';
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'q' FROM generate_series(153001, 153100) i; PREPARE TRANSACTION 'hf_four';
SELECT sift.col_promote('events_c') >= 0 AS promoted;
COMMIT PREPARED 'hf_three';
ROLLBACK PREPARED 'hf_four';
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('two: one committed, one rolled back');
REINDEX INDEX events_c;
SELECT unstamped() AS unstamped;

-- a merge settles what it takes in: no step between the commit and the merge, and the merged segment is stamped
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'm' FROM generate_series(160001, 160300) i; PREPARE TRANSACTION 'hf_merge';
INSERT INTO events SELECT i, i % 10, i, 'c' FROM generate_series(161001, 161200) i;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
COMMIT PREPARED 'hf_merge';
SELECT sift.col_merge('events_c', true) >= 0 AS merged;
SELECT unstamped() AS unstamped, (SELECT count(*) FROM sift.col_stamps('events_c')) AS segments;
SELECT * FROM chk('merged after the commit');

-- a merge while the transaction is still open: the segment it writes waits too, and the commit settles it
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'm' FROM generate_series(170001, 170300) i; PREPARE TRANSACTION 'hf_merge_open';
INSERT INTO events SELECT i, i % 10, i, 'c' FROM generate_series(171001, 171200) i;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT sift.col_merge('events_c', true) >= 0 AS merged;
SELECT unstamped() AS unstamped, (SELECT count(*) FROM sift.col_stamps('events_c')) AS segments;
SELECT * FROM chk('merged while open');
COMMIT PREPARED 'hf_merge_open';
SELECT * FROM chk('merged while open, then committed');
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('merged while open, settled');

-- ... and when it rolls back the merged segment says nothing, whatever merges follow
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'm' FROM generate_series(180001, 180300) i; PREPARE TRANSACTION 'hf_merge_abort';
INSERT INTO events SELECT i, i % 10, i, 'c' FROM generate_series(181001, 181200) i;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT sift.col_merge('events_c', true) >= 0 AS merged;
ROLLBACK PREPARED 'hf_merge_abort';
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('merged while open, then rolled back');
INSERT INTO events SELECT i, i % 10, i, 'c' FROM generate_series(182001, 182200) i;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT sift.col_merge('events_c', true) >= 0 AS merged;
SELECT unstamped() AS unstamped, (SELECT count(*) FROM sift.col_stamps('events_c')) AS segments;
SELECT * FROM chk('merged after the rollback');

-- transactions in doubt more than 63 xids apart are more than a stamp waits for: no stamp, the same answers
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'o' FROM generate_series(190001, 190100) i; PREPARE TRANSACTION 'hf_old';
\set ECHO none
\o /dev/null
SELECT format('INSERT INTO events VALUES (%s, 1, 1, ''t'')', 191000 + i) FROM generate_series(1, 70) i \gexec
\o
\set ECHO all
BEGIN; INSERT INTO events SELECT i, i % 10, i, 'n' FROM generate_series(192001, 192100) i; PREPARE TRANSACTION 'hf_new';
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
COMMIT PREPARED 'hf_old';
COMMIT PREPARED 'hf_new';
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('too far apart, committed');

REINDEX INDEX events_c;
SELECT unstamped() AS unstamped;
SELECT * FROM chk('rebuilt');

DROP FUNCTION unstamped();
DROP FUNCTION chk(text);
DROP FUNCTION pages();
DROP FUNCTION answer(bool);
DROP TABLE events;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
