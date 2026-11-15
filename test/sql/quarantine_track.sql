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
SELECT expect($$ DELETE FROM t WHERE id BETWEEN 200 AND 300 $$) AS delete;
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

DROP TABLE t;
DROP FUNCTION expect(text), plan_of(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
