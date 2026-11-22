-- Format 1 sift_table VACUUM: a plain VACUUM cuts the free tail of the file when most of the rows are gone, the group of a column segment with no live row is dropped
-- whether or not it was frozen, and vacuum_truncate = false keeps the file. Every check prints one row (name, ok).
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE v1_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE v1_k (id int, v text) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false);
INSERT INTO v1_t SELECT g, repeat(md5(g::text), 3) FROM generate_series(1, 40000) g;
INSERT INTO v1_k SELECT g, repeat(md5(g::text), 3) FROM generate_series(1, 40000) g;
VACUUM v1_t;
VACUUM v1_k;
SELECT pg_relation_size('v1_t') AS t_loaded, pg_relation_size('v1_k') AS k_loaded \gset
DELETE FROM v1_t;
DELETE FROM v1_k;
VACUUM v1_t;
VACUUM v1_t;
VACUUM v1_k;
VACUUM v1_k;
SELECT 'plain VACUUM shrinks the file', pg_relation_size('v1_t') < :t_loaded * 0.5 AS ok;
SELECT 'vacuum_truncate = false keeps it', pg_relation_size('v1_k') >= :k_loaded * 0.5 AS ok;
SELECT 'no row is left', (SELECT count(*) FROM v1_t) = 0 AND (SELECT count(*) FROM v1_k) = 0 AS ok;
INSERT INTO v1_t SELECT g, 'again ' || g FROM generate_series(1, 1000) g;
SELECT 'it is a format 1 table', count(*) > 0 AS ok FROM sift.table_segments('v1_t');
SELECT 'the table takes rows again', count(*) = 1000 AS ok FROM v1_t;

DROP TABLE v1_t, v1_k;
DROP EXTENSION pg_sift;
