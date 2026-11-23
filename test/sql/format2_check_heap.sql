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

DROP TABLE hc;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
