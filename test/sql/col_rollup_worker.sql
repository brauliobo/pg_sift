-- The background worker of the on-demand rollup refresh (docs/COL-PREAGG.md section 7) serves a queue of several segments and ends: the requests a SELECT queued are
-- refreshed by the worker and the next SELECT answers from the rollups. The worker is the only process that allocates a name for each request it serves, so this is
-- the test that runs that loop (the others serve their queue in the session, sift.async_maintenance off) and the one the leak gate of memcheck runs over it.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.test_limits = 'par_build_rows=1';
SET client_min_messages = warning;
SET sift.agg_rollup_cost = 0;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
SET sift.async_maintenance = off;

CREATE TEMP TABLE rw_base AS SELECT requested, refreshed FROM sift.col_rollup_state;
CREATE VIEW rw_state AS SELECT s.requested - b.requested AS requested, s.refreshed - b.refreshed AS refreshed FROM sift.col_rollup_state s, rw_base b;
CREATE VIEW rw_requests AS SELECT * FROM sift.col_rollup_requests WHERE index::text IN ('rw_a_i', 'rw_b_i');

-- two indexes, each connected to a parent (the worker reads the connection leaf of each into the relcache entry it builds and ends with both): the queue holds one request
-- for the one segment of each, and the worker serves them one after the other
CREATE TABLE rw_p (id int PRIMARY KEY, v int);
INSERT INTO rw_p SELECT g, g % 7 FROM generate_series(1, 50) g;
CREATE INDEX rw_p_i ON rw_p USING sift_col (id, v);
CREATE TABLE rw_a (id int, p int, lines jsonb, pad text) WITH (autovacuum_enabled = off);
CREATE TABLE rw_b (LIKE rw_a) WITH (autovacuum_enabled = off);
INSERT INTO rw_a
SELECT g, 1 + g % 50, (SELECT jsonb_agg(jsonb_build_object('a', 'a' || (g + e) % 5, 'b', 'b' || (g * e) % 3, 'q', (g * e) % 9) ORDER BY e) FROM generate_series(1, g % 4 + 1) e), repeat('p', 100)
FROM generate_series(1, 400) g;
INSERT INTO rw_b SELECT * FROM rw_a;
SELECT format('CREATE INDEX %I ON %I USING sift_col (id, p, lines) WITH (paths = %L, connect = ''p -> rw_p(id)'', write_buffer_limit = 4096, rollup_leaves = ''off'')', t || '_i', t,
              'lines[]' || chr(123) || 'a text, b text, q int4' || chr(125)) FROM unnest(ARRAY['rw_a', 'rw_b']) t \gexec
SELECT format('VACUUM (ANALYZE, FREEZE, DISABLE_PAGE_SKIPPING) %I', t) FROM unnest(ARRAY['rw_a', 'rw_b']) t \gexec
SELECT format('ALTER INDEX %I SET (rollup_leaves = ''lines[].a, lines[].b'')', t || '_i') FROM unnest(ARRAY['rw_a', 'rw_b']) t \gexec

CREATE FUNCTION rw_plan(tbl text, OUT units int, OUT pre int, OUT queued int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT e.a, e.b, count(*), sum(e.q) FROM ' || tbl || ' d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, q int4) GROUP BY 1, 2' LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from '(\d+) from pre-aggregates')::int, pre);
		queued := coalesce(substring(l from '(\d+) refreshes queued')::int, queued);
	END LOOP;
END $$;

-- the units have no rollup: the SELECT queues their segments
SELECT t, pre = 0 AS none_from_rollups, queued = units AS all_queued FROM unnest(ARRAY['rw_a', 'rw_b']) t, rw_plan(t);
SELECT count(*) AS segments_waiting FROM rw_requests WHERE state = 'queued';

-- a worker serves the queue and ends (the queue is empty and no worker is left)
SELECT sift._test_col_rollup_worker() AS worker_started;
CREATE FUNCTION rw_served() RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..600 LOOP
		IF NOT EXISTS (SELECT 1 FROM rw_requests) AND NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE backend_type = 'sift rollup refresh' AND datname = current_database()) THEN RETURN true; END IF;
		PERFORM pg_sleep(0.1);
	END LOOP;
	RETURN false;
END $$;
SELECT rw_served() AS worker_ended;
SELECT (SELECT count(*) FROM rw_requests) AS left_in_queue, (SELECT requested > 0 AND refreshed = requested FROM rw_state) AS all_refreshed;
SELECT t, sift.col_verify(t || '_i', true) FROM unnest(ARRAY['rw_a', 'rw_b']) t;
SELECT t, pre = units AS all_from_rollups, queued FROM unnest(ARRAY['rw_a', 'rw_b']) t, rw_plan(t);

DROP TABLE rw_a, rw_b, rw_p;
DROP VIEW rw_requests, rw_state;
DROP FUNCTION rw_plan(text), rw_served();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
