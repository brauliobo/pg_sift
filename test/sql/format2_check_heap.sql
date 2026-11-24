-- sift.check(index, heapallindexed) on a Format 2.0 sift_col index compares the addresses of the rows the heap shows to a snapshot with those the index returns: on an empty index and an
-- empty heap (nothing to sort), on rows in a micro-slab and in parts, after DELETE and UPDATE, and many times in one transaction (a scan the check leaves open is a warning at its commit), and after TRUNCATE.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE hc (id int PRIMARY KEY, n int);
CREATE INDEX hc_idx ON hc USING sift_col (id, n);
SELECT count(*) AS problems FROM sift.check('hc_idx', heapallindexed => true);
SELECT sift.col_verify('hc_idx', true);

INSERT INTO hc VALUES (1, 1);
SELECT count(*) AS problems FROM sift.check('hc_idx', heapallindexed => true);
INSERT INTO hc SELECT i, i % 10 FROM generate_series(2, 5000) i;
SELECT count(*) AS problems FROM sift.check('hc_idx', heapallindexed => true);
DELETE FROM hc WHERE id % 7 = 0;
UPDATE hc SET n = n + 1 WHERE id % 5 = 0;
SELECT count(*) AS problems FROM sift.check('hc_idx', heapallindexed => true);
SELECT sift.col_verify('hc_idx', true);
SELECT sift.verify('hc_idx', true);

BEGIN;
SELECT count(*) AS problems FROM generate_series(1, 200) i, LATERAL (SELECT 1 FROM sift.check('hc_idx', heapallindexed => true) LIMIT 1) c;
COMMIT;

TRUNCATE hc;
SELECT count(*) AS problems FROM sift.check('hc_idx', heapallindexed => true);

-- an index that follows no changes holds the rows of the versions an UPDATE or DELETE left until its VACUUM: the heap not showing them is no problem
CREATE TABLE hc_off (id int PRIMARY KEY, n int) WITH (autovacuum_enabled = false);
INSERT INTO hc_off SELECT i, i FROM generate_series(1, 3000) i;
CREATE INDEX hc_off_idx ON hc_off USING sift_col (id, n) WITH (track_changes = off);
UPDATE hc_off SET n = n + 1 WHERE id % 3 = 0;
DELETE FROM hc_off WHERE id % 7 = 0;
SELECT count(*) AS problems FROM sift.check('hc_off_idx', heapallindexed => true);
SELECT count(*) FROM hc_off;
SELECT count(*) AS problems FROM sift.check('hc_off_idx', heapallindexed => true);

-- a row the heap shows and the index lacks is a row of sift.check each time it is asked: the report takes no index out of service
UPDATE pg_index SET indisready = false WHERE indexrelid = 'hc_off_idx'::regclass;
INSERT INTO hc_off VALUES (5000, 1);
UPDATE pg_index SET indisready = true WHERE indexrelid = 'hc_off_idx'::regclass;
SELECT problem, regexp_replace(detail, '\([0-9]+,[0-9]+\)', '(b,o)') AS detail FROM sift.check('hc_off_idx', heapallindexed => true);
SELECT problem, regexp_replace(detail, '\([0-9]+,[0-9]+\)', '(b,o)') AS detail FROM sift.check('hc_off_idx', heapallindexed => true);
SELECT count(*) AS in_service FROM hc_off WHERE id = 5000;

DROP TABLE hc, hc_off;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
