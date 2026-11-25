-- The queue of the background maintenance (core/maint/sched.h): a request for an index of format major 2 is run by a worker of the server in a transaction of its own and leaves the queue empty;
-- with sift.async_maintenance off nothing is queued.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SET sift.async_maintenance = on;
CREATE FUNCTION completed_by(n int8, seconds int) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	stop timestamptz := clock_timestamp() + make_interval(secs => seconds);
BEGIN
	WHILE clock_timestamp() < stop LOOP
		IF (SELECT completed FROM sift._test_maint_stats()) >= n THEN RETURN true; END IF;
		PERFORM pg_sleep(0.05);
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE ms (id int, v int);
INSERT INTO ms SELECT g, g % 7 FROM generate_series(1, 5000) g;
CREATE INDEX ms_col ON ms USING sift_col (id, v);
SELECT completed + 1 AS target FROM sift._test_maint_stats() \gset

SELECT sift._test_maint_enqueue('ms_col', 3);
SELECT completed_by(:target, 30) AS worker_ran;
SELECT pending FROM sift._test_maint_stats();
SELECT count(*) FROM sift_internal.maint_queue();
SELECT sift.verify('ms_col');

SET sift.async_maintenance = off;
SELECT sift._test_maint_enqueue('ms_col', 3);

DROP TABLE ms;
DROP FUNCTION completed_by(int8, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
