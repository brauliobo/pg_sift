-- Grouping sets that all lie inside one of them are summed from its groups: the scan updates the finest set and the others are added from it at the end
-- (EXPLAIN ANALYZE says "Sets Summed From One"). The rows equal those of the normal plan, with NULL keys, the grand total of an empty input, workers and units
-- answered from pre-aggregates; a query with a float sum or a count(DISTINCT) keeps one pass per set.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE sf_events (id int PRIMARY KEY, dim_a_id int, country text, category text, status text, amount numeric(10,2), qty int, score float8, created date)
	WITH (autovacuum_enabled = off);
INSERT INTO sf_events
SELECT g, g % 211, CASE WHEN g % 17 = 0 THEN NULL ELSE 'c' || g % 6 END, CASE WHEN g % 13 = 0 THEN NULL ELSE 'k' || g % 4 END, (ARRAY['paid', 'open', 'void'])[1 + g % 3],
       CASE WHEN g % 29 = 0 THEN NULL ELSE (g % 997) / 7.0 END, CASE WHEN g % 23 = 0 THEN NULL ELSE g % 31 END, (g % 101) / 8.0, date '2025-01-01' + g % 90
FROM generate_series(1, 40000) g;
CREATE INDEX sf_events_col ON sf_events USING sift_col (id, dim_a_id, country, category, status, amount, qty, score, created) WITH (rollup_dims = 'country, category, status');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) sf_events;
SET sift.agg_force = on;

CREATE FUNCTION sf_rows(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN scan THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT string_agg(t::text, E''\n'' ORDER BY t::text) FROM (%s) t', q) INTO r;
	RETURN r;
END $$;

-- the sets summed from one, and none for a query that cannot add its states
CREATE FUNCTION sf_summed(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		n := coalesce(substring(l from 'Sets Summed From One: (\d+)')::int, n);
	END LOOP;
	RETURN n;
END $$;

CREATE TABLE sf_queries (n int, q text);
INSERT INTO sf_queries VALUES
 (1, 'SELECT country, category, count(*), sum(amount), avg(amount), min(qty), max(qty) FROM sf_events GROUP BY GROUPING SETS ((country, category), (country), (category), ())'),
 (2, 'SELECT country, category, status, count(*), sum(qty), count(amount), grouping(country, category, status) FROM sf_events GROUP BY ROLLUP (country, category, status)'),
 (3, 'SELECT country, status, count(amount), avg(qty), min(amount), max(amount) FROM sf_events WHERE qty > 5 GROUP BY CUBE (country, status)'),
 (4, 'SELECT country, count(*), sum(amount) FROM sf_events WHERE id < 0 GROUP BY ROLLUP (country)'),
 (5, 'SELECT country, category, sum(score), count(*) FROM sf_events GROUP BY ROLLUP (country, category)'),
 (6, 'SELECT country, count(DISTINCT dim_a_id), count(*) FROM sf_events GROUP BY ROLLUP (country)'),
 (7, 'SELECT created, country, count(*), sum(amount) FROM sf_events WHERE created >= date ''2025-02-01'' GROUP BY GROUPING SETS ((created, country), (created), (country))'),
 (8, 'SELECT country, category, count(*) FROM sf_events GROUP BY GROUPING SETS ((country), (category))');

SELECT n, sf_rows(q, true) = sf_rows(q, false) AS same_rows, length(sf_rows(q, false)) > 0 AS has_rows, sf_summed(q) AS summed FROM sf_queries ORDER BY n;

SET sift.agg_preagg = off;
SELECT n, sf_rows(q, true) = sf_rows(q, false) AS same_rows, sf_summed(q) AS summed FROM sf_queries WHERE n IN (1, 2, 3, 7) ORDER BY n;

SET sift.agg_preagg = on;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 3;
SELECT n, sf_rows(q, true) = sf_rows(q, false) AS same_rows FROM sf_queries WHERE n IN (1, 2, 3, 7) ORDER BY n;

-- the workers took part
CREATE FUNCTION sf_workers(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		n := coalesce(substring(l from 'Workers Launched: (\d+)')::int, n);
	END LOOP;
	RETURN n;
END $$;
SELECT sf_workers(q) > 0 AS workers, sf_summed(q) AS summed FROM sf_queries WHERE n = 1;

DROP FUNCTION sf_workers, sf_summed, sf_rows;
DROP TABLE sf_queries, sf_events;
DROP EXTENSION pg_sift;
