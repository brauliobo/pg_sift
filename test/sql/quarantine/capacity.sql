-- The registry of damaged indexes has a fixed number of entries (quarantine_entries, lowered to 2 here). A full registry gives up the entry of an index that a worker has flagged in its
-- metapage and keeps the entry of one that has no flag (the index of a temporary table gets none): every damaged index stays out of service and the newest damage is listed
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
CREATE FUNCTION flagged_in(idx regclass, seconds int) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	stop timestamptz := clock_timestamp() + make_interval(secs => seconds);
BEGIN
	WHILE clock_timestamp() < stop LOOP
		IF sift._meta_quarantined(idx) THEN RETURN true; END IF;
		PERFORM pg_sleep(0.05);
	END LOOP;
	RETURN false;
END $$;
SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
SET sift.test_limits = 'quarantine_entries=2';

CREATE TEMP TABLE a (id int, v int, g int);
CREATE TABLE b (id int, v int, g int);
CREATE TABLE c (id int, v int, g int);
INSERT INTO a SELECT i, i % 7, i % 3 FROM generate_series(1, 20000) i;
INSERT INTO b SELECT i, i % 7, i % 3 FROM generate_series(1, 20000) i;
INSERT INTO c SELECT i, i % 7, i % 3 FROM generate_series(1, 20000) i;
CREATE INDEX a_idx ON a USING sift_col (id, v, g);
CREATE INDEX b_idx ON b USING sift_col (id, v, g);
CREATE INDEX c_idx ON c USING sift_col (id, v, g);
ANALYZE a;
ANALYZE b;
ANALYZE c;

-- the oldest entry is the index of the temporary table, which no worker flags; the next is flagged
SELECT sift._test_poke('a_idx', 3, 8176, 9, 2);
SELECT sift_helpers.statement_outcome($$ SELECT count(*) FROM a WHERE v = 3 $$, ': ') AS detecting_a;
SELECT sift._test_poke('b_idx', 3, 8176, 9, 2);
SELECT sift_helpers.statement_outcome($$ SELECT count(*) FROM b WHERE v = 3 $$, ': ') AS detecting_b;
SELECT flagged_in('b_idx', 20) AS b_flagged;
SELECT index, persisted FROM sift.quarantine ORDER BY 1;

-- the registry is full: the third damage takes the entry of the flagged index, not that of the one with no flag
SELECT sift._test_poke('c_idx', 3, 8176, 9, 2);
SELECT sift_helpers.statement_outcome($$ SELECT count(*) FROM c WHERE v = 3 $$, ': ') AS detecting_c;
SELECT flagged_in('c_idx', 20) AS c_flagged;
SELECT index, persisted, detected_at IS NOT NULL AS in_registry FROM sift.quarantine ORDER BY 1;

-- every damaged index is out of service, listed by its entry or by its flag: no plan reads it and the answers are the heap's
EXPLAIN (COSTS OFF) SELECT count(*) FROM a WHERE v = 3;
EXPLAIN (COSTS OFF) SELECT count(*) FROM b WHERE v = 3;
EXPLAIN (COSTS OFF) SELECT count(*) FROM c WHERE v = 3;
SELECT (SELECT count(*) FROM a WHERE v = 3) AS a_rows, (SELECT count(*) FROM b WHERE v = 3) AS b_rows, (SELECT count(*) FROM c WHERE v = 3) AS c_rows;

RESET sift.test_limits;
DROP TABLE a, b, c;
DROP FUNCTION flagged_in(regclass, int);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
