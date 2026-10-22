-- The settings (sift.*) as ordinary roles: the ones for tuning a session can be set, the ones that change what a query may see or how an index
-- is written are for superusers (or for a role that was granted SET on the parameter), and the settings a role keeps with ALTER ROLE follow
-- the same rule.
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
\set ECHO none
\ir test/roles_helpers.sql
\set ECHO all
SELECT rr_roles_drop('rs_');
CREATE ROLE rs_own;
CREATE ROLE rs_user;
CREATE ROLE rs_grant;
CREATE SCHEMA rs;
GRANT USAGE ON SCHEMA rs TO rs_user, rs_grant;
GRANT USAGE, CREATE ON SCHEMA rs TO rs_own;
GRANT USAGE ON SCHEMA sift TO rs_own, rs_user, rs_grant;
SET enable_seqscan = off;

-- every setting of the extension, set to its reset value by an ordinary role: the outcome follows the context of the setting
CREATE FUNCTION rs_try() RETURNS TABLE(name text, context text, outcome text) LANGUAGE plpgsql AS $$
DECLARE s record;
BEGIN
	FOR s IN SELECT p.name, p.context, p.reset_val FROM pg_settings p WHERE p.name LIKE 'sift.%' ORDER BY p.name LOOP
		name := s.name; context := s.context;
		BEGIN
			SET LOCAL SESSION AUTHORIZATION rs_user;
			PERFORM set_config(s.name, s.reset_val, true);
			outcome := 'ok';
		EXCEPTION WHEN OTHERS THEN
			outcome := 'error ' || SQLSTATE;
		END;
		RESET SESSION AUTHORIZATION;
		RETURN NEXT;
	END LOOP;
END $$;
SELECT context, outcome FROM rs_try() GROUP BY 1, 2 ORDER BY 1, 2;
SELECT name, context FROM rs_try() WHERE context NOT IN ('user', 'superuser', 'sighup') ORDER BY 1;

-- the settings that matter most, with the error
SELECT * FROM rr_run($$
rs_user | SET sift.enable_scan = off
rs_user | SET sift.enable_agg = off
rs_user | SET sift.enable_count = off
rs_user | SET sift.enable_topn = off
rs_user | SET sift.agg_lookup_max_rows = 1000
rs_user | SET sift.agg_rollup_cost = 2.5
rs_user | SET sift.table_backpressure = 100
rs_user | SET sift.enable_scan = 'maybe'
rs_user | SET sift.rls_scores = on
rs_user | SET sift.col_format_minor = 4
rs_user | SET sift.async_maintenance = off
rs_user | SET sift.maintenance_cost_delay = 0
rs_user | SET sift.maintenance_naptime = 500
rs_user | SET sift.max_expansions = 10
rs_user | SET sift.log_min_duration = 0
rs_user | SET sift.standby_conflicts = off
rs_user | SET sift.col_auto_merge = on
rs_user | SET LOCAL sift.rls_scores = on
rs_user | SELECT set_config('sift.rls_scores', 'on', false)
rs_user | SELECT set_config('sift.enable_scan', 'on', false)
rs_user | RESET sift.rls_scores
rs_user | RESET sift.enable_scan
rs_user | SHOW sift.rls_scores
rs_user | SELECT current_setting('sift.col_format_minor') IS NOT NULL
rs_user | ALTER ROLE rs_user SET sift.enable_scan = off
rs_user | ALTER ROLE rs_user SET sift.rls_scores = on
rs_user | ALTER ROLE rs_user RESET ALL
rs_user | ALTER ROLE rs_grant SET sift.enable_scan = off
rs_user | ALTER DATABASE postgres SET sift.enable_scan = off
$$);

-- GRANT SET ON PARAMETER hands a restricted setting to one role
GRANT SET ON PARAMETER sift.rls_scores, sift.col_format_minor TO rs_grant;
SELECT * FROM rr_run($$
rs_grant | SET sift.rls_scores = on
rs_grant | SET sift.col_format_minor = 4
rs_grant | SET sift.async_maintenance = off
rs_grant | RESET sift.rls_scores
rs_user | SET sift.rls_scores = on
$$);

-- a setting changes the plan of the session that sets it
SELECT * FROM rr_run($$
rs_own | CREATE TABLE rs.docs (id int, body text, g int)
rs_own | INSERT INTO rs.docs SELECT i, 'alpha beta ' || i, i % 3 FROM generate_series(1, 300) i
rs_own | CREATE INDEX docs_idx ON rs.docs USING sift (body)
rs_own | GRANT SELECT ON rs.docs TO rs_user, rs_grant
rs_user | EXPLAIN SELECT id FROM rs.docs WHERE body @@> 'alpha'
rs_user | SET sift.enable_scan = off
rs_user | EXPLAIN SELECT id FROM rs.docs WHERE body @@> 'alpha'
rs_user | SELECT count(*) FROM rs.docs WHERE body @@> 'alpha'
rs_user | RESET sift.enable_scan
rs_user | EXPLAIN SELECT id FROM rs.docs WHERE body @@> 'alpha'
$$);

DROP SCHEMA rs CASCADE;
DROP FUNCTION rs_try();
DROP FUNCTION rr_run(text), rr_matrix(text[], text), rr_roles_drop(text);
DROP OWNED BY rs_own, rs_user, rs_grant;
DROP ROLE rs_own, rs_user, rs_grant;
SELECT count(*) AS parameter_grants_left FROM pg_parameter_acl WHERE parname LIKE 'sift.%';
DROP EXTENSION pg_sift;
