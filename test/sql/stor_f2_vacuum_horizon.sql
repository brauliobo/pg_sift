-- Format 2.0 sift_table VACUUM: a plain VACUUM (and autovacuum) cuts the free tail of the file like VACUUM (TRUNCATE true), the reloption vacuum_truncate = false and
-- VACUUM (TRUNCATE false) keep it; a deletion vector settles and a pending delete folds at the oldest snapshot of the vacuum, not 50 million transactions behind.
-- Every check prints one row (name, ok).
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION loaded(rel text, n int) RETURNS bigint LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('INSERT INTO %I SELECT g, repeat(md5(g::text), 4) FROM generate_series(1, %s) g', rel, n);
	RETURN pg_relation_size(rel);
END $$;

CREATE TABLE tr_plain (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE tr_off (id int, v text) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false);
CREATE TABLE tr_opt (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
SELECT loaded('tr_plain', 60000) AS plain_loaded \gset
SELECT loaded('tr_off', 60000) AS off_loaded \gset
SELECT loaded('tr_opt', 60000) AS opt_loaded \gset
DELETE FROM tr_plain WHERE id > 6000;
DELETE FROM tr_off WHERE id > 6000;
DELETE FROM tr_opt WHERE id > 6000;
VACUUM tr_plain;
VACUUM tr_plain;
VACUUM tr_plain;
VACUUM tr_off;
VACUUM tr_off;
VACUUM tr_off;
VACUUM (TRUNCATE false) tr_opt;
VACUUM (TRUNCATE false) tr_opt;
VACUUM (TRUNCATE false) tr_opt;
SELECT 'plain VACUUM gives the tail back', pg_relation_size('tr_plain') < :plain_loaded AS ok;
SELECT 'vacuum_truncate = false keeps it', pg_relation_size('tr_off') > pg_relation_size('tr_plain') AS ok;
SELECT 'VACUUM (TRUNCATE false) keeps it', pg_relation_size('tr_opt') > pg_relation_size('tr_plain') AS ok;
VACUUM (TRUNCATE true) tr_opt;
VACUUM (TRUNCATE true) tr_opt;
SELECT 'VACUUM (TRUNCATE true) cuts it', pg_relation_size('tr_opt') < pg_relation_size('tr_off') AS ok;
SELECT 'the live rows are all there', (SELECT count(*) FROM tr_plain) = 6000 AND (SELECT count(*) FROM tr_off) = 6000 AND (SELECT count(*) FROM tr_opt) = 6000 AS ok;

-- the deletes of a committed transaction are settled and folded by the next plain VACUUM, so that a second VACUUM finds nothing to write
CREATE TABLE hz (id int, v text) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false);
INSERT INTO hz SELECT g, md5(g::text) FROM generate_series(1, 30000) g;
VACUUM hz;
DELETE FROM hz WHERE id % 3 = 0;
VACUUM hz;
SELECT pg_current_wal_lsn() AS lsn \gset
VACUUM hz;
SELECT 'a second VACUUM writes little', pg_wal_lsn_diff(pg_current_wal_lsn(), :'lsn') < 20000 AS ok;
SELECT 'the rows left', count(*) = 20000 AS ok FROM hz;

DROP TABLE tr_plain, tr_off, tr_opt, hz;
DROP FUNCTION loaded(text, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
