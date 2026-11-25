-- Whatever sift reports while it holds the relation extension lock, it reports without a lock of the catalog: PostgreSQL forbids a heavyweight lock under the extension lock (an assertion with
-- cassert), and the error context of the index and the damage record look names up. Every class of core/damage.h, and what is no damage, raised under the lock with a cold catalog cache.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE;
END $$;

SET client_min_messages = error;
CREATE TABLE t (id int, v int, body text);
INSERT INTO t SELECT g, g % 7, 'alpha beta' || g % 5 FROM generate_series(1, 2000) g;
CREATE INDEX t_col ON t USING sift_col (id, v);
CREATE INDEX t_bm25 ON t USING sift (body);

SELECT m AS on_corruption, i AS index, k AS kind, expect(format('SET sift.on_corruption = %L; SELECT sift._test_raise_extending(%L::regclass, %L)', m, i, k)) AS raised
FROM unnest(ARRAY['error', 'quarantine']) m, unnest(ARRAY['t_col', 't_bm25']) i,
	 unnest(ARRAY['index', 'data', 'scope', 'other_data', 'other_index', 'error', 'warning', 'log']) k
ORDER BY m DESC, i, k;
-- the same with a write that extends the relation under the lock
SELECT m AS on_corruption, i AS index, k AS kind, expect(format('SET sift.on_corruption = %L; SELECT sift._test_raise_extending(%L::regclass, %L, true)', m, i, k)) AS raised
FROM unnest(ARRAY['error', 'quarantine']) m, unnest(ARRAY['t_col', 't_bm25']) i,
	 unnest(ARRAY['index', 'data', 'scope', 'other_data', 'other_index', 'error', 'warning', 'log']) k
ORDER BY m DESC, i, k;
SELECT index, detections > 0 AS recorded FROM sift.quarantine ORDER BY index;

DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
