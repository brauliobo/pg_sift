-- An index that tracks changes of its table with triggers leaves service like any other: the writes that fire the triggers go on, the rows are exact, and the rebuild
-- brings the index back with the rows written while it was out
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
	p text := '';
BEGIN
	FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		p := p || r || E'\n';
	END LOOP;
	RETURN CASE WHEN p ~ 'Sift|t_idx' THEN 'sift' ELSE 'heap' END;
END $$;
SET client_min_messages = error;
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int PRIMARY KEY, v int);
INSERT INTO t SELECT i, i % 7 FROM generate_series(1, 20000) i;
CREATE INDEX t_idx ON t USING sift_col (id, v);
ANALYZE t;
SELECT count(*) AS triggers FROM pg_trigger WHERE tgrelid = 't'::regclass AND tgname LIKE 'sift%';

SELECT sift._test_poke('t_idx', 2, 8176, 9, 2);
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS detecting_statement;
SELECT expect($$ UPDATE t SET v = 9 WHERE id <= 100 $$) AS update;
SELECT expect($$ DELETE FROM t WHERE id BETWEEN 200 AND 300 $$) AS delete_records;
SELECT expect($$ DELETE FROM t WHERE id BETWEEN 400 AND 405 $$) AS delete_marks;
SELECT expect($$ UPDATE t SET id = id + 100000 WHERE id BETWEEN 500 AND 510 $$) AS update_the_key;
SELECT expect($$ INSERT INTO t VALUES (30000, 1) $$) AS insert;
SELECT expect($$ UPDATE t SET v = 8 WHERE id = 30000 $$) AS update_the_new_row;
VACUUM t;
SELECT count(*) AS rows_now, count(*) FILTER (WHERE v = 9) AS nines, count(*) FILTER (WHERE v = 8) AS eights FROM t;
SELECT plan_of('SELECT count(*) FROM t WHERE v = 3') AS plan_while_out;

REINDEX INDEX t_idx;
ANALYZE t;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
SELECT count(*) AS rows_now, count(*) FILTER (WHERE v = 9) AS nines, count(*) FILTER (WHERE v = 8) AS eights FROM t;
SET sift.on_corruption = error;
SELECT count(*) AS threes FROM t WHERE v = 3;
SELECT count(*) AS check_problems FROM sift.check('t_idx', heapallindexed => true);


-- the write that meets the damage first: the statement trigger of a key column and the row trigger of ctid leave the index out and the statement goes on
SET sift.on_corruption = quarantine;
CREATE TABLE u (id int PRIMARY KEY, v int);
CREATE TABLE w (id int PRIMARY KEY, v int);
INSERT INTO u SELECT i, i % 7 FROM generate_series(1, 20000) i;
INSERT INTO w SELECT i, i % 7 FROM generate_series(1, 20000) i;
CREATE INDEX u_idx ON u USING sift_col (id, v);
CREATE INDEX w_idx ON w USING sift_col (id, v) WITH (track_changes = ctid);
SELECT sift._test_poke('u_idx', 2, 8176, 9, 2);
SELECT sift._test_poke('w_idx', 2, 8176, 9, 2);
SELECT expect(format($$ %s $$, q)) AS first_write FROM unnest(ARRAY[
	'DELETE FROM u WHERE id BETWEEN 1 AND 300', 'UPDATE u SET id = id + 100000 WHERE id BETWEEN 301 AND 310', 'INSERT INTO u VALUES (30000, 1)',
	'DELETE FROM w WHERE id BETWEEN 1 AND 300', 'UPDATE w SET v = 9 WHERE id BETWEEN 301 AND 310', 'INSERT INTO w VALUES (30000, 1)']) q;
SELECT index, skipped_writes > 0 AS skipped FROM sift.quarantine WHERE index = 'u_idx';
SELECT expect($$ SELECT count(*) FROM w WHERE v = 3 $$) AS detecting_statement;
SELECT expect($$ DELETE FROM w WHERE id BETWEEN 400 AND 700 $$) AS delete_after_detection;
REINDEX INDEX u_idx;
REINDEX INDEX w_idx;
SET sift.on_corruption = error;
SELECT (SELECT count(*) FROM u) AS u_rows, (SELECT count(*) FROM w) AS w_rows;
SELECT (SELECT count(*) FROM sift.check('u_idx', heapallindexed => true)) + (SELECT count(*) FROM sift.check('w_idx', heapallindexed => true)) AS check_problems;
DROP TABLE u, w;

DROP TABLE t;
DROP FUNCTION expect(text), plan_of(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
