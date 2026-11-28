-- VACUUM FULL and CLUSTER of a sift_table keep the rows whose delete VACUUM settled out: a deletion vector entry without a deleter is a row dead for every snapshot, as for the
-- heap twin, and the rewrite reads the deleter of a row as the scans do.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET sift.test_create_format1 = off;

CREATE TABLE vs_t (pk int PRIMARY KEY, v int) USING sift_table;
CREATE TABLE vs_h (pk int PRIMARY KEY, v int);
INSERT INTO vs_t SELECT i, i FROM generate_series(1, 10000) i;
INSERT INTO vs_h SELECT i, i FROM generate_series(1, 10000) i;
VACUUM vs_t;
DELETE FROM vs_t WHERE pk % 100 = 0;
DELETE FROM vs_h WHERE pk % 100 = 0;
VACUUM vs_t;
SELECT count(*) AS sift, (SELECT count(*) FROM vs_h) AS heap FROM vs_t;
VACUUM FULL vs_t;
VACUUM FULL vs_h;
SELECT count(*) AS sift, (SELECT count(*) FROM vs_h) AS heap FROM vs_t;
SELECT (SELECT count(*) FROM (TABLE vs_t EXCEPT ALL TABLE vs_h) x) AS sift_only, (SELECT count(*) FROM (TABLE vs_h EXCEPT ALL TABLE vs_t) x) AS heap_only;

DELETE FROM vs_t WHERE pk % 100 = 1;
DELETE FROM vs_h WHERE pk % 100 = 1;
VACUUM vs_t;
CLUSTER vs_t USING vs_t_pkey;
CLUSTER vs_h USING vs_h_pkey;
SELECT count(*) AS sift, (SELECT count(*) FROM vs_h) AS heap FROM vs_t;
SELECT (SELECT count(*) FROM (TABLE vs_t EXCEPT ALL TABLE vs_h) x) AS sift_only, (SELECT count(*) FROM (TABLE vs_h EXCEPT ALL TABLE vs_t) x) AS heap_only;
SELECT index, segment, problem, detail FROM sift.check('vs_t');

DROP TABLE vs_t, vs_h;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
