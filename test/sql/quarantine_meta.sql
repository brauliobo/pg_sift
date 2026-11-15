-- An index whose metapage is damaged cannot say anything of itself: it is quarantined by what the metapage shows, and DROP INDEX, REINDEX, REINDEX CONCURRENTLY and
-- the functions that report on indexes still work
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;
SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int);
INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 5000) g;
CREATE INDEX t_idx ON t USING sift_col (id, v);
CREATE INDEX t_bm ON t USING btree (v);

-- the magic number of the metapage
SELECT sift._test_poke('t_idx', 0, 24, 0, 4);
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS detecting_statement;
SELECT index, error_code, reason FROM sift.quarantine;
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS next_statement;
SELECT expect($$ INSERT INTO t VALUES (5001, 1) $$) AS insert;
SELECT expect($$ UPDATE t SET v = 2 WHERE id = 7 $$) AS update;
SELECT expect($$ DELETE FROM t WHERE id = 8 $$) AS delete;
VACUUM t;
SELECT count(*) AS problems FROM sift.check('t_idx');

REINDEX INDEX t_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
SELECT sift._test_poke('t_idx', 0, 24, 0, 4);
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS detecting_again;
REINDEX INDEX CONCURRENTLY t_idx;
SELECT count(*) AS listed_after_concurrently FROM sift.quarantine;
SELECT sift._test_poke('t_idx', 0, 24, 0, 4);
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS detecting_a_third_time;
SELECT expect($$ DROP INDEX t_idx $$) AS drop_index;
SELECT count(*) AS listed_after_drop FROM sift.quarantine;

DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
