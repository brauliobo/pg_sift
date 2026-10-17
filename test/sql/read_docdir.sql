-- The SB directory of a segment's docmap (24 bytes per superblock, read and summed by every query that reads the
-- documents of the segment) is kept by the backend: the second query of a ranked scan reads none of it again
-- (sift._test_read_stats counts the bytes), while a rewritten segment (merge, promotion) has its own.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- 7 superblocks of 256 pages
CREATE TABLE rd (id serial, body text, pad text);
INSERT INTO rd (body, pad) SELECT 'w' || (g % 50) || ' x' || (g % 7) || ' common', repeat('p', 1500) FROM generate_series(1, 9000) g;
CREATE INDEX rd_idx ON rd USING sift (body);
ANALYZE rd;
SELECT pg_relation_size('rd') / 8192 / 256 AS superblocks;

CREATE FUNCTION rd_top(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT string_agg(id || '':'' || s, '','' ORDER BY n) FROM (SELECT row_number() OVER () AS n, id, s FROM
		(SELECT id, sift.score(ctid) s FROM rd WHERE body @@> $1 ORDER BY sift.score(ctid) DESC, id LIMIT 25) x) y' INTO r USING q;
	RETURN r;
END $$;
SELECT sum(ndocs) > 0 AS segments_have_docs FROM sift.segments('rd_idx'::regclass) WHERE kind = 'immutable';

-- the answers do not depend on the directory being kept
SET sift.test_docdir_min_sb = 100000;
CREATE TEMP TABLE rd_ref AS SELECT q, rd_top(q) AS a FROM unnest(ARRAY['w1', 'w2 OR x3', 'common x1', 'w49 x6']) q;

-- directories of at least 2 superblocks are kept
SET sift.test_docdir_min_sb = 2;
SELECT count(*) AS reset FROM sift._test_read_stats(true);
SELECT q FROM rd_ref WHERE rd_top(q) IS DISTINCT FROM a;
SELECT value AS first_bytes FROM sift._test_read_stats() WHERE key = 'docdir_bytes_read' \gset
SELECT count(*) AS reset FROM sift._test_read_stats(true);
SELECT q FROM rd_ref WHERE rd_top(q) IS DISTINCT FROM a;
SELECT :first_bytes > 0 AS first_queries_read_the_directory,
	   (SELECT value FROM sift._test_read_stats() WHERE key = 'docdir_bytes_read') AS second_queries_bytes,
	   (SELECT value FROM sift._test_read_stats() WHERE key = 'docdir_hits') > 0 AS found_in_cache;
RESET sift.test_docdir_min_sb;

DROP FUNCTION rd_top(text);
DROP TABLE rd;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
