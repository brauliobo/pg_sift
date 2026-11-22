-- A vacuum moves what pins the end of the file below the free space under it, projection parts and the pages of the directory included, so that the file can be cut: after a bulk delete the
-- relation shrinks, the projections still answer, and the structure of the index checks.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
\i test/proj_helpers.sql

CREATE TABLE t (id bigint, g int, v bigint, pad text);
CREATE INDEX t_idx ON t USING sift_col (id, g, v, pad) WITH (key_order = 'id');
INSERT INTO t SELECT i, i % 20, i % 1000, repeat(md5(i::text), 4) FROM generate_series(1, 200000) i;
VACUUM t;
SELECT sift.create_projection('t_idx', 't_agg', 'GROUP BY g MEASURES count(*), sum(v)');
SELECT sift.create_projection('t_idx', 't_sort', 'ORDER BY g, v, id');
VACUUM t;

-- Rows written after the projections, behind everything else in the file
INSERT INTO t SELECT 1000000 + i, i % 20, i % 1000, repeat('z', 40) FROM generate_series(1, 20000) i;
VACUUM t;
CREATE TABLE t_size AS SELECT pg_relation_size('t_idx') AS before_bytes;

-- The bulk of the file is deleted: the free space is under the projection parts and the directory that were written last
DELETE FROM t WHERE id <= 150000;
VACUUM t;
VACUUM t;
SELECT p_check('the file was cut', pg_relation_size('t_idx') < (SELECT before_bytes FROM t_size) * 0.6);
SELECT p_check('agg answers', p_same('SELECT g, count(*), sum(v) FROM t GROUP BY g') = 'same');
SELECT p_check('sort answers', p_same_order('SELECT id FROM t WHERE g = 7 ORDER BY v, id LIMIT 30', 'SELECT id FROM t WHERE g = 7 ORDER BY v + 0, id LIMIT 30') = 'same');
SELECT p_check('agg served', p_uses('SELECT g, count(*), sum(v) FROM t GROUP BY g', 't_agg'));
SELECT p_check('sort served', p_uses('SELECT id FROM t WHERE g = 7 ORDER BY v, id LIMIT 30', 't_sort'));
SELECT count(*) FROM sift.check('t_idx', false, 100);

-- Dropping a projection gives its pages back
SELECT sift.drop_projection('t_idx', 't_sort');
VACUUM t;
SELECT p_check('still answers', p_same('SELECT g, count(*), sum(v) FROM t GROUP BY g') = 'same');
SELECT count(*) FROM sift.check('t_idx', false, 100);

DROP TABLE t, t_size;
DROP FUNCTION p_check(text, boolean), p_rows(text, boolean), p_rows_ordered(text, boolean), p_same(text), p_same_order(text, text), p_uses(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
