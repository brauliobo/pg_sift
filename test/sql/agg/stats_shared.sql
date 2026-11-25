-- sift.stats() returns the counters of the server (summed over its backends, in the shared memory) and sift.stats(local => true) those of the session: the same rows,
-- the totals include what the session counted and do not go down when it resets its own. Another session's share of the totals is tested in test/t/agg/stats_shared.sh.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

CREATE TABLE sh (id int, grp int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO sh SELECT i, i % 7, i % 100 FROM generate_series(0, 19999) i;
CREATE INDEX sh_col ON sh USING sift_col (id, grp, amount);
CREATE TABLE plain (g int);
VACUUM (FREEZE, ANALYZE) sh;

CREATE FUNCTION total(n text) RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce((SELECT value FROM sift.stats() WHERE name = n), 0) $$;
CREATE FUNCTION mine(n text) RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce((SELECT value FROM sift.stats(local => true) WHERE name = n), 0) $$;

-- the shape
SELECT p.oid::regprocedure AS function, pg_get_function_arguments(p.oid) AS arguments, pg_get_function_result(p.oid) AS result FROM pg_proc p WHERE pronamespace = 'sift'::regnamespace AND proname = 'stats' ORDER BY pronargs;
SELECT count(*) AS strict_null_rows FROM sift.stats(NULL);
SELECT sift.stats_reset();
-- the same counters in the same order
SELECT (SELECT array_agg(name) FROM sift.stats() WHERE name !~ '^(refused|limit)[.]') = (SELECT array_agg(name) FROM sift.stats(local => true) WHERE name !~ '^(refused|limit)[.]') AS same_counters;
SELECT count(*) > 30 AS counters FROM sift.stats(local => true);

-- a scan is counted in the session and in the totals
SELECT total('scans') AS before \gset
SELECT count(*) FROM (SELECT grp, count(*) FROM sh GROUP BY grp) g;
SELECT mine('scans') AS session_scans, mine('units') > 0 AS session_units;
SELECT total('scans') - :before >= 1 AS totals_counted, total('units') >= mine('units') AS totals_hold_the_session;

-- the refusals too, by slug
SET sift.count_refusals = on;
SELECT g, count(*) FROM plain GROUP BY g;
RESET sift.count_refusals;
SELECT mine('refused.no_source') AS session_refused, total('refused.no_source') >= 1 AS totals_refused;
SELECT name FROM sift.stats(local => true) WHERE name LIKE 'refused.%' OR name LIKE 'limit.%' ORDER BY name;

-- the reset empties the session and leaves the totals
SELECT total('scans') AS kept \gset
SELECT sift.stats_reset();
SELECT mine('scans') AS session_scans, total('scans') >= :kept AS totals_kept;
SELECT count(*) AS session_refusals FROM sift.stats(local => true) WHERE name LIKE 'refused.%';
SELECT total('refused.no_source') >= 1 AS totals_refused_kept;

DROP FUNCTION total(text), mine(text);
DROP TABLE sh, plain;
DROP EXTENSION pg_sift CASCADE;
