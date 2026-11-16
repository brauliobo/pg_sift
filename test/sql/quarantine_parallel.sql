-- Damage that a parallel worker meets: the leader that runs no part of the plan (parallel_leader_participation = off) gets the damage error once, from the worker, and the statements that
-- follow are planned on the heap and give its rows. The worker records the index in the registry; the metapage is flagged by the worker that the next plan asks for, as the leader
-- did not see the damage itself. Both workers open the index: the one that opens it after the other recorded the damage refuses the plan (40001, retry, core/quarantine.c), and the leader
-- reads the error of the first worker that failed, so the statement fails once with either code; the registry holds the damage code in both.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION diag(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	c text;
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS c = PG_EXCEPTION_CONTEXT;
	RETURN CASE WHEN SQLSTATE IN ('XX002', '40001') THEN 'XX002 or 40001' ELSE SQLSTATE END || ' from ' || CASE WHEN c ~ 'parallel worker' THEN 'a parallel worker' ELSE 'the leader' END;
END $$;
CREATE FUNCTION flagged_in(seconds int) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	stop timestamptz := clock_timestamp() + make_interval(secs => seconds);
BEGIN
	WHILE clock_timestamp() < stop LOOP
		PERFORM count(*) FROM p WHERE g = 2;
		IF (SELECT bool_and(persisted) FROM sift.quarantine) THEN RETURN true; END IF;
		PERFORM pg_sleep(0.05);
	END LOOP;
	RETURN false;
END $$;

SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET parallel_leader_participation = off;
CREATE TABLE p (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false, parallel_workers = 2);
INSERT INTO p SELECT i, i % 5, i FROM generate_series(1, 40000) i;
CREATE INDEX p_c ON p USING sift_col (g, id, v);
VACUUM (FREEZE, ANALYZE) p;
CREATE TABLE answer AS SELECT count(*) AS n, sum(v) AS total FROM p WHERE g = 2;
SELECT (SELECT count(*) FROM sift.quarantine) AS listed_before;
EXPLAIN (COSTS OFF) SELECT count(*), sum(v) FROM p WHERE g = 2;

SELECT sift._test_poke('p_c', 2, 8176, 9, 2);
SELECT diag($$ SELECT count(*), sum(v) FROM p WHERE g = 2 $$) AS detecting_statement;
SELECT index, error_code, detections FROM sift.quarantine;
SELECT count(*), sum(v) FROM p WHERE g = 2 EXCEPT SELECT n, total FROM answer;
EXPLAIN (COSTS OFF) SELECT count(*), sum(v) FROM p WHERE g = 2;
SELECT flagged_in(20) AS flagged_by_the_worker_the_plan_asked_for;

REINDEX INDEX p_c;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;

DROP TABLE p, answer;
DROP FUNCTION diag(text), flagged_in(int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
