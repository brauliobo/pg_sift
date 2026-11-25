-- The SQL functions of the extension fix their search_path with pg_temp last: a temporary table named like a catalog view is not found before it (PostgreSQL searches the temporary schema first
-- for relations and types unless the path names it), and every function that sets a path sets one that ends in pg_temp. The two quantile functions set one too.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE facts (id int, value_num int) USING sift_table;
CREATE TEMP TABLE pg_settings AS SELECT name, source, 'TROJAN'::text AS boot_val FROM pg_catalog.pg_settings;
SELECT count(*) > 0 AS options, count(*) FILTER (WHERE default_value = 'TROJAN') AS read_from_temp_table FROM sift.table_options('facts');
DROP TABLE pg_temp.pg_settings;

SELECT p.oid::regprocedure AS without_pg_temp
FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
WHERE n.nspname IN ('sift', 'sift_internal')
  AND EXISTS (SELECT 1 FROM unnest(p.proconfig) c WHERE c LIKE 'search_path=%')
  AND NOT EXISTS (SELECT 1 FROM unnest(p.proconfig) c WHERE c ~ '^search_path=.*, pg_temp$')
ORDER BY 1::text;

SELECT p.proname, p.proconfig FROM pg_proc p WHERE p.pronamespace = 'sift'::regnamespace AND p.proname IN ('approx_percentile', 'approx_quantiles') ORDER BY 1;

DROP TABLE facts;
DROP EXTENSION pg_sift;
