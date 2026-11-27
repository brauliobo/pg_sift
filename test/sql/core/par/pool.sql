-- The pool of compute threads, the driver and the grouped aggregation of core/par and agg/par (src/core/par/test.c): every run gives every group and every row once, on one, two and four threads, in the
-- two regimes; and a statement that is cancelled (at the n-th unit of the call, or by statement_timeout) leaves the pool idle and its memory freed, so that the next statement runs as the first did.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 3;

SELECT w AS workers, g AS groups, r.threads = w AS used_all, r.runs, r.out_groups, r.bad
FROM (VALUES (1), (2), (4)) AS v(w), (VALUES (100), (3000), (50000)) AS x(g),
     LATERAL sift._test_par_group(w, 200000, g, 3, 0) AS r
ORDER BY w, g;

-- the statement is cancelled at the n-th unit of work of the call, whichever thread takes it: the statement ends with a cancel, 30 times in a row, and every run after it is complete
DO $$
DECLARE
	cancelled int := 0;
	good int := 0;
BEGIN
	FOR i IN 1..30 LOOP
		BEGIN
			PERFORM sift._test_par_group(4, 300000, 50000, 5, 1 + i % 7);
		EXCEPTION WHEN query_canceled THEN
			cancelled := cancelled + 1;
		END;
		IF (SELECT bad = 0 AND runs = 2 FROM sift._test_par_group(4, 100000, 20000, 2, 0)) THEN
			good := good + 1;
		END IF;
	END LOOP;
	RAISE NOTICE 'cancelled %, complete runs after it %', cancelled, good;
END $$;

-- a statement that runs out of time, three times, each followed by a complete run
SET statement_timeout = '60ms';
SELECT * FROM sift._test_par_group(4, 1000000, 100000, 100000, 0);
RESET statement_timeout;
SELECT bad, runs FROM sift._test_par_group(4, 100000, 20000, 2, 0);
SET statement_timeout = '60ms';
SELECT * FROM sift._test_par_group(2, 1000000, 100000, 100000, 0);
RESET statement_timeout;
SELECT bad, runs FROM sift._test_par_group(2, 100000, 20000, 2, 0);
SET statement_timeout = '60ms';
SELECT * FROM sift._test_par_group(4, 1000000, 1000, 100000, 0);
RESET statement_timeout;
SELECT bad, runs FROM sift._test_par_group(4, 100000, 20000, 2, 0);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
