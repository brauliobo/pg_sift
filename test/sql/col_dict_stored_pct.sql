-- The reloption dict_stored: a dictionary of 1,024 strings or more is written uncompressed while that adds at most this percent to the segment; 0 compresses it. The answers
-- do not depend on it, the size does.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE t (id int, word text, n int, r int) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT i, 'word number ' || (i % 5000) || ' of the long common phrase', i % 10, hashint4(i) FROM generate_series(1, 200000) i;
CREATE INDEX t_zero ON t USING sift_col (id, word, n, r) WITH (dict_stored = 0);
CREATE INDEX t_wide ON t USING sift_col (id, word, n, r) WITH (dict_stored = 50);
SELECT pg_relation_size('t_wide') > pg_relation_size('t_zero') AS stored_is_larger;
SELECT pg_relation_size('t_wide') < pg_relation_size('t_zero') * 1.6 AS but_bounded;
SET sift.agg_force = on;
SELECT count(*), sum(n) FROM (SELECT word, sum(n) AS n FROM t GROUP BY 1) s;
DROP INDEX t_zero;
SELECT count(*), sum(n) FROM (SELECT word, sum(n) AS n FROM t GROUP BY 1) s;
ALTER INDEX t_wide SET (dict_stored = 101);
DROP TABLE t;
DROP EXTENSION pg_sift;
