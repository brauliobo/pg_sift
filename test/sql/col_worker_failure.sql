-- A background worker of a sift_col index that fails keeps the error where sift.workers shows it (the steps of a column index run outside a
-- subtransaction; the worker's transaction is the unit), and the next successful step clears it. The worker reads the setting of the database.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SELECT current_database() AS db \gset
ALTER DATABASE :"db" SET sift.test_fail_pages = 1;
CREATE TABLE cw (id serial, v int, t text) WITH (autovacuum_enabled = off);
CREATE INDEX cw_idx ON cw USING sift_col (v, t) WITH (write_buffer_limit = 64);
SELECT failures, error_code, last_error FROM sift.workers WHERE index = 'cw_idx'::regclass;

-- the rows go in and settle in this session, the setting being the database's and this session older than it: a step of this session that
-- ends after the worker has failed would count as the next successful step and clear what the worker keeps
SET sift.async_maintenance = off;
INSERT INTO cw (v, t) SELECT g, md5(g::text) FROM generate_series(1, 30000) g;
SELECT sift.col_promote('cw_idx') >= 0 AS settled;
RESET sift.async_maintenance;

-- a few rows seal a write buffer and ask for a worker, whose promotion fails; nothing of this session runs a step meanwhile
INSERT INTO cw (v, t) SELECT g, md5(g::text) FROM generate_series(30001, 31500) g;
DO $$
BEGIN
	FOR i IN 1..300 LOOP
		EXIT WHEN (SELECT failures FROM sift.workers WHERE index = 'cw_idx'::regclass) > 0;
		PERFORM pg_sleep(0.1);
	END LOOP;
END $$;
SELECT kind, failures, error_code, last_error, next_retry > failed_at AS retry_later FROM sift.workers WHERE index = 'cw_idx'::regclass;
SELECT count(*) FROM cw WHERE v > 0;

-- the failure is not in the way of the index: a promotion in this session (the setting is the database's, this session began before it) clears it
SELECT sift.col_promote('cw_idx') >= 0 AS promoted;
SELECT failures, last_error FROM sift.workers WHERE index = 'cw_idx'::regclass;
SELECT sift.col_verify('cw_idx');

ALTER DATABASE :"db" RESET sift.test_fail_pages;
DROP TABLE cw;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
