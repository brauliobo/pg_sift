-- The flag of the metapage is written by a worker after the statement that found the damage: sift.quarantine says persisted when it is there, and a rebuilt index has none
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
CREATE FUNCTION persisted_in(seconds int) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	stop timestamptz := clock_timestamp() + make_interval(secs => seconds);
BEGIN
	WHILE clock_timestamp() < stop LOOP
		IF (SELECT bool_and(persisted) FROM sift.quarantine) THEN RETURN true; END IF;
		PERFORM pg_sleep(0.05);
	END LOOP;
	RETURN false;
END $$;
SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int, g int);
INSERT INTO t SELECT i, i % 7, i % 3 FROM generate_series(1, 20000) i;
CREATE INDEX t_idx ON t USING sift_col (id, v, g);
ANALYZE t;

SELECT sift._test_poke('t_idx', 2, 8176, 9, 2);
SELECT sift_helpers.statement_outcome($$ SELECT count(*) FROM t WHERE v = 3 $$, ': ') AS detecting_statement;
SELECT index, persisted IS NOT NULL AS listed FROM sift.quarantine;
SELECT persisted_in(20) AS flagged_by_worker;
SELECT sift_helpers.statement_outcome($$ INSERT INTO t VALUES (20001, 1, 1) $$, ': ') AS insert_after_flag;

REINDEX INDEX t_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;

DROP TABLE t;
DROP FUNCTION persisted_in(int);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
