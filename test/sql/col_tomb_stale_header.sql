-- The completeness record of a tombstone log is in the header page of the log, and a header written before the record has fewer used bytes: whatever the page holds past them is
-- not a record. A page can hold one nonetheless (a page that held a complete log before and is given to another header), and a log taken for complete on its word would make a read
-- skip the heap for deletes it never heard. The oracle is the heap: the reads are the same with and without the log's say.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO events SELECT i, i % 10, i, 'x' FROM generate_series(1, 20000) i;
CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_updates = 'id', track_deletes = on);
VACUUM events;
SELECT track_deletes, triggers, complete, heap_free FROM sift.col_tracking('events_c');
SELECT * FROM sift._test_tomb_header('events_c');

-- the page of a release before the record, with the bytes of a complete record still on it
SELECT * FROM sift._test_tomb_header('events_c', true, true);
SELECT track_deletes, triggers, complete, heap_free FROM sift.col_tracking('events_c');
-- the reads: an update of a column the index does not cover clears the visibility map and writes nothing to the log (the page of the header is not written again); the log is not known
-- to be complete, so the heap checks and the answer is the table's
UPDATE events SET label = 'y' WHERE id % 13 = 0;
SET sift.enable_agg = on;
SELECT sift._test_hf_taken(true) AS reset;
SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY tenant_id ORDER BY 1;
SELECT sift._test_hf_taken() AS taken_from_the_log;
SET sift.enable_agg = off;
SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY tenant_id ORDER BY 1;

-- a new baseline writes the record again
RESET sift.enable_agg;
ALTER INDEX events_c RESET (track_deletes);
ALTER INDEX events_c SET (track_deletes = on);
SELECT * FROM sift._test_tomb_header('events_c');
SELECT track_deletes, triggers, complete, heap_free FROM sift.col_tracking('events_c');
DROP TABLE events;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
