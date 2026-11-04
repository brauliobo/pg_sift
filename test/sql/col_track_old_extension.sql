-- track_changes needs sift.col_track_old_rows(), which the extension has from 4.0 on. pg_upgrade restores a cluster of an older release with the extension at its old version
-- (ALTER EXTENSION UPDATE is the user's next step) and the library already this release's, and the restore makes the indexes: the default (auto) must leave the index without
-- the log, as it does when the table has no key, and a column the user names must be refused with the way out, not fail on a missing function. The state of that extension is made
-- in a transaction that ends in a rollback: the function dropped from it.
CREATE EXTENSION pg_sift;
SET client_min_messages = notice;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE a (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO a SELECT i, i % 5, i FROM generate_series(1, 200) i;

BEGIN;
ALTER EXTENSION pg_sift DROP FUNCTION sift.col_track_old_rows();
DROP FUNCTION sift.col_track_old_rows();

CREATE INDEX a_auto ON a USING sift_col (id, g, v);
SELECT track_changes, triggers FROM sift.col_tracking('a_auto');
SELECT tgname FROM pg_trigger WHERE tgrelid = 'a'::regclass AND NOT tgisinternal;

SAVEPOINT named;
CREATE INDEX a_named ON a USING sift_col (id, g, v) WITH (track_changes = 'id');
ROLLBACK TO named;

CREATE INDEX a_off ON a USING sift_col (id, g, v) WITH (track_changes = 'off');
REINDEX INDEX a_auto;
SELECT track_changes, triggers FROM sift.col_tracking('a_auto');
ROLLBACK;

-- the extension as it is: the same statements track
CREATE INDEX a_auto ON a USING sift_col (id, g, v);
SELECT track_changes, triggers FROM sift.col_tracking('a_auto');

DROP TABLE a;
DROP EXTENSION pg_sift;
