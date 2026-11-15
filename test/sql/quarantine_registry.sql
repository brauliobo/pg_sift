-- Damage found by a sift_col index is recorded in the registry of the server; sift.on_corruption = error records nothing; a function that reports damage does not record it;
-- an index rebuilt by REINDEX is a new file and leaves the list
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;

SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int);
INSERT INTO t SELECT g, g % 7 FROM generate_series(1, 20000) g;
CREATE INDEX t_idx ON t USING sift_col (id, v);
SELECT count(*) AS listed FROM sift.quarantine;

-- the data page of the segment gets another page type
SELECT sift._test_poke('t_idx', 2, 8176, 9, 2);
SELECT expect($$ SELECT sift.col_verify('t_idx') $$);
SELECT count(*) AS listed_after_verify FROM sift.quarantine;
SELECT count(*) AS problems FROM sift.check('t_idx');
SELECT count(*) AS listed_after_check FROM sift.quarantine;

SET sift.on_corruption = error;
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$);
SELECT count(*) AS listed_in_error_mode FROM sift.quarantine;

SET sift.on_corruption = quarantine;
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$);
SELECT index, error_code, reason, detections, skipped_writes, skipped_vacuums, refused_scans FROM sift.quarantine;

REINDEX INDEX t_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
