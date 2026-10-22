-- The rollup option and the on-demand refresh of the rollups (docs/COL-PREAGG.md section 7). `rollup = 'leaf, leaf'` names the dims of the rollups of
-- an index; a SELECT that scans exact units for lack of their rollup (none, other dims, a measure it lacks) queues the segment of each and does not write; the
-- queue is served by a background worker, here by sift.col_rollup_refresh (sift.col_rollup_worker off), and the next SELECT answers from the rollups.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
-- these tests are about the rollup path: it answers whatever the cost of its tuples (agg_rollup_cost has its own test)
SET sift.agg_rollup_cost = 0;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
SET sift.col_rollup_worker = off;

-- the counters and the queue are the server's (other tests ask too): the counters are read as the change since here, the queue as the requests of the index
CREATE TEMP TABLE rr_base AS SELECT requested, refreshed, skipped FROM sift.col_rollup_state;
CREATE VIEW rr_state AS SELECT s.requested - b.requested AS requested, s.refreshed - b.refreshed AS refreshed, s.skipped - b.skipped AS skipped
	FROM sift.col_rollup_state s, rr_base b;
CREATE VIEW rr_requests AS SELECT * FROM sift.col_rollup_requests WHERE index::text = 'rr_i';

CREATE TABLE rr (id int, lines jsonb, pad text) WITH (autovacuum_enabled = off);
INSERT INTO rr
SELECT g, CASE WHEN g % 10 = 0 THEN NULL ELSE
       (SELECT jsonb_agg(jsonb_build_object('a', 'a' || (g + e) % 5, 'b', 'b' || (g * e) % 3, 'c', (g + 2 * e) % 3, 'q', (g * e) % 9, 'w', ((g * e) % 40) * 0.25) ORDER BY e)
        FROM generate_series(1, g % 4 + 1) e) END, repeat('p', 100)
FROM generate_series(1, 30000) g;

-- plan of a query: units, those answered from the rollups, those scanned for lack of it, those queued, the dims read
CREATE FUNCTION rr_plan(q text, OUT units int, OUT pre int, OUT miss int, OUT queued int, OUT dims text) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
		miss := coalesce(substring(l from 'Units Without The Rollup: (\d+)')::int, miss);
		queued := coalesce(substring(l from 'Rollup Refresh Queued: (\d+)')::int, queued);
		dims := coalesce(substring(l from 'Pre-aggregate Dims: (.*)$'), dims);
	END LOOP;
END $$;

-- the rows of a query from the rollups and from the plain plan
CREATE FUNCTION rr_same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO a;
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO b;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN a IS NOT DISTINCT FROM b;
END $$;

CREATE TABLE rr_q (n int, q text);
INSERT INTO rr_q VALUES
	(1, 'SELECT e.a, e.b, count(*), sum(e.q) FROM rr d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8) GROUP BY 1, 2'),
	(2, 'SELECT e.a, e.b, e.c, count(*), min(e.w), max(e.w) FROM rr d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8) GROUP BY 1, 2, 3'),
	(3, 'SELECT e.b, count(*), sum(e.q) FROM rr d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8) WHERE e.a IN (''a1'', ''a2'') GROUP BY 1'),
	(4, 'SELECT e.q, e.a, count(*) FROM rr d CROSS JOIN LATERAL jsonb_to_recordset(d.lines) e(a text, b text, c int2, q int4, w float8) GROUP BY 1, 2');

-- the names of the option: leaves of array elements or map values declared in paths, low cardinality, at most 6 per list
CREATE INDEX rr_bad ON rr USING sift_col (id, lines) WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8}', rollup = 'lines[].nosuch');
CREATE INDEX rr_bad ON rr USING sift_col (id, lines) WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8}', rollup = 'id');
CREATE INDEX rr_bad ON rr USING sift_col (id, lines) WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8}', rollup = 'lines[].w');
CREATE INDEX rr_bad ON rr USING sift_col (id, lines) WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8}', rollup = 'lines[].a, lines[].a');
CREATE INDEX rr_bad ON rr USING sift_col (id, lines) WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8}', rollup = 'lines[].a, , lines[].b');
CREATE INDEX rr_bad ON rr USING sift_col (id, lines) WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8}', rollup = 'lines[].a: lines[].q');
CREATE INDEX rr_bad ON rr USING sift_col (id, lines) WITH (paths = 'lines[]{a text, b text, c int2, d int2, e int2, f int2, g int2, h int2}', rollup = 'lines[].a, lines[].b, lines[].c, lines[].d, lines[].e, lines[].f, lines[].g');
SELECT count(*) AS indexes FROM pg_class WHERE relname = 'rr_bad';

-- no rollup at all: nothing to answer from, nothing to ask for
CREATE INDEX rr_i ON rr USING sift_col (id, lines) WITH (paths = 'lines[]{a text, b text, c int2, q int4, w float8}', write_buffer_limit = 4096, rollup = 'off');
VACUUM (ANALYZE, FREEZE, DISABLE_PAGE_SKIPPING) rr;
SELECT units > 2 AS units FROM rr_plan((SELECT q FROM rr_q WHERE n = 1));
SELECT (SELECT requested FROM rr_state) AS requested, (SELECT count(*) FROM rr_requests) AS queued;

-- the units are clean but have no rollup: a SELECT scans them and queues their segments; its rows are those of the plain plan
ALTER INDEX rr_i SET (rollup = 'lines[].a, lines[].b');
ALTER INDEX rr_i SET (rollup = 'lines[].nosuch');
ALTER INDEX rr_i SET (rollup = 'lines[].w');
SELECT reloptions FROM pg_class WHERE relname = 'rr_i';
SELECT pre = 0 AS none_from_rollups, miss = units AS all_missing, queued = units AS all_queued, dims FROM rr_plan((SELECT q FROM rr_q WHERE n = 1));
SELECT rr_same(q) AS same FROM rr_q WHERE n = 1;
SELECT (SELECT requested FROM rr_state) > 0 AS requested, (SELECT count(*) FROM rr_requests WHERE state = 'queued') > 0 AS waiting,
       (SELECT sum(units) FROM rr_requests) = (SELECT requested FROM rr_state) AS units_add_up,
       (SELECT refreshed + skipped FROM rr_state) AS done;
-- the queue is read only: no segment changed
SELECT count(DISTINCT gen) AS segments FROM sift.col_segments('rr_i') WHERE kind = 'immutable';

-- the refresh rewrites the segments; the units answer
SELECT sift.col_rollup_refresh('rr_i') > 0 AS refreshed;
SELECT (SELECT count(*) FROM rr_requests) AS queued, (SELECT requested = refreshed + skipped FROM rr_state) AS all_done,
       (SELECT skipped FROM rr_state) AS skipped;
SELECT sift.col_verify('rr_i', true);
SELECT pre = units AS all_from_rollups, miss AS missing, queued, dims FROM rr_plan((SELECT q FROM rr_q WHERE n = 1));
SELECT rr_same(q) AS same FROM rr_q WHERE n = 1;
-- nothing is asked for again: the segments are written with the rollups
SELECT sift.col_rollup_refresh('rr_i') AS refreshed_again;

-- a subset of the dims of the units is answered; a dim they lack is asked for
SELECT pre = units AS all_from_rollups, dims FROM rr_plan((SELECT q FROM rr_q WHERE n = 3));
ALTER INDEX rr_i SET (rollup = 'lines[].a, lines[].b, lines[].c');
SELECT pre = 0 AS none_from_rollups, miss = units AS all_missing, queued = units AS all_queued, dims FROM rr_plan((SELECT q FROM rr_q WHERE n = 2));
SELECT pre = units AS all_from_rollups, dims FROM rr_plan((SELECT q FROM rr_q WHERE n = 1));
SELECT rr_same(q) AS same FROM rr_q WHERE n = 2;
SELECT sift.col_rollup_refresh('rr_i') > 0 AS refreshed;
SELECT pre = units AS all_from_rollups, miss AS missing, dims FROM rr_plan((SELECT q FROM rr_q WHERE n = 2));
SELECT rr_same(q) AS same FROM rr_q ORDER BY n;

-- the switches: the setting, the option of the index
ALTER INDEX rr_i SET (rollup = 'lines[].a, lines[].b, lines[].c, lines[].q');
SET sift.col_rollup_on_demand = off;
SELECT miss = units AS all_missing, queued FROM rr_plan((SELECT q FROM rr_q WHERE n = 4));
RESET sift.col_rollup_on_demand;
ALTER INDEX rr_i SET (rollup_on_demand = false);
SELECT miss = units AS all_missing, queued FROM rr_plan((SELECT q FROM rr_q WHERE n = 4));
ALTER INDEX rr_i SET (rollup_on_demand = true);
SELECT count(*) AS queued FROM rr_requests;
-- a read-only transaction asks too, and writes nothing
BEGIN READ ONLY;
SELECT rr_same(q) AS same FROM rr_q WHERE n = 4;
COMMIT;
SELECT count(*) > 0 AS queued FROM rr_requests;
SELECT sift.col_rollup_refresh('rr_i') > 0 AS refreshed;
SELECT pre = units AS all_from_rollups, dims FROM rr_plan((SELECT q FROM rr_q WHERE n = 4));

-- units on dirty pages are not asked for: a refresh would not make them exact
UPDATE rr SET lines = '[{"a": "a1", "b": "b1", "c": 1, "q": 5, "w": 1.5}]' WHERE id <= 3000;
SELECT pre > 0 AS some_from_rollups, pre < units AS some_scanned, miss AS missing, queued FROM rr_plan((SELECT q FROM rr_q WHERE n = 4));
SELECT rr_same(q) AS same FROM rr_q ORDER BY n;
SELECT count(*) AS queued FROM rr_requests;

DROP TABLE rr, rr_q;
DROP VIEW rr_requests, rr_state;
DROP FUNCTION rr_plan(text), rr_same(text);
DROP EXTENSION pg_sift;
