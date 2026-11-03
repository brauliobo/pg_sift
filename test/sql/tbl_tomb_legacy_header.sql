-- The header page of a tombstone log has a completeness record since 4.0 (used by a column index with track_changes; a sift_table has none). A log written before it has a header with fewer
-- used bytes, and the bytes past them are whatever the page held: the record is read only where the used bytes cover it. The log of a sift_table whose header is such a page keeps hiding
-- its deleted rows, and the log VACUUM writes in its place has a header of the new length with an empty record.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
CREATE TABLE t (id int PRIMARY KEY, v int) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE h (id int PRIMARY KEY, v int) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, i FROM generate_series(1, 5000) i;
INSERT INTO h SELECT * FROM t;
DELETE FROM t WHERE id % 5 = 0;
DELETE FROM h WHERE id % 5 = 0;
SELECT * FROM sift._test_tomb_header('t');

-- the page of an older release
SELECT * FROM sift._test_tomb_header('t', true);
SELECT (SELECT count(*) || '/' || sum(v) FROM t) = (SELECT count(*) || '/' || sum(v) FROM h) AS same_after_legacy_header;
DELETE FROM t WHERE id % 7 = 0;
DELETE FROM h WHERE id % 7 = 0;
UPDATE t SET v = v + 1 WHERE id % 11 = 0;
UPDATE h SET v = v + 1 WHERE id % 11 = 0;
SELECT (SELECT count(*) || '/' || sum(v) FROM t) = (SELECT count(*) || '/' || sum(v) FROM h) AS same_after_more_changes;

-- the log VACUUM writes has the new header
VACUUM t;
VACUUM h;
SELECT * FROM sift._test_tomb_header('t');
SELECT (SELECT count(*) || '/' || sum(v) FROM t) = (SELECT count(*) || '/' || sum(v) FROM h) AS same_after_vacuum;

DROP TABLE t;
DROP TABLE h;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
