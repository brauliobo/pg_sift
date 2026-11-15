-- The dimension map for every kind of column of the parent: an int2, int4, int8, date, timestamp, boolean, text (dictionary), numeric and an expression leaf of its sift_col index,
-- joined by a child whose own filter and group keys are leaves of the child's sift_col index. For each kind, whether the join is served by a map ('new map' made by the statement, 'shared map' made by an earlier one), by the classes of
-- the lookup table ('classes') or loads the rows of the parent ('rows'), and how many heap fetches it made, before and after writes to the parent and to the child, serial and with
-- workers, with and without a connection of the child to the parent's journal (`via map`: nothing is carried). The answers are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE TABLE pd (k int PRIMARY KEY, c2 int2, c4 int4, c8 int8, cd date, cts timestamp, cb bool, ct text, cn numeric(10,2)) WITH (autovacuum_enabled = false);
INSERT INTO pd SELECT i, i % 7, i % 11, (i % 13) * 10000000000, date '2024-01-01' + i % 17, timestamp '2024-01-01' + (i % 19) * interval '1 hour', i % 3 = 0, 't' || i % 23,
	(i % 29) / 10.0 FROM generate_series(0, 4999) i;
CREATE INDEX pd_col ON pd USING sift_col (k, c2, c4, c8, cd, cts, cb, ct, cn, ((c4 * 100 + c2)));
CREATE TABLE ch (id int PRIMARY KEY, k int, g int, v int, tag text) WITH (autovacuum_enabled = false);
INSERT INTO ch SELECT i, i % 5100, i % 5, i % 101, 'u' || i % 4 FROM generate_series(1, 40000) i;
CREATE INDEX ch_col ON ch USING sift_col (id, k, g, v, tag);
VACUUM (FREEZE, ANALYZE) pd, ch;

CREATE TABLE attrs (n int, name text, expr text);
INSERT INTO attrs VALUES (1, 'int2', 'pd.c2'), (2, 'int4', 'pd.c4'), (3, 'int8', 'pd.c8'), (4, 'date', 'pd.cd'), (5, 'timestamp', 'pd.cts'), (6, 'boolean', 'pd.cb'),
	(7, 'text', 'pd.ct'), (8, 'numeric', 'pd.cn'), (9, 'expression', '(pd.c4 * 100 + pd.c2)');

CREATE FUNCTION q(expr text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT 'SELECT ' || expr || ', ch.g, ch.tag, count(*), sum(ch.v) FROM ch JOIN pd ON pd.k = ch.k WHERE ch.v < 50 GROUP BY 1, 2, 3 ORDER BY 1, 2, 3'
$$;
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
-- how the scan got the parent's columns, from EXPLAIN ANALYZE: a map, the classes of the lookup table, the rows of it; and the heap fetches it made
CREATE FUNCTION how(sql text, OUT served text, OUT heap_fetches int) LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	served := 'normal'; heap_fetches := 0;
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || sql LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN served := 'rows'; END IF;
		IF l ~ '[1-9]\d* rows classed' AND served = 'rows' THEN served := 'classes'; END IF;
		IF l ~ '[1-9]\d* maps shared' THEN served := 'shared map'; END IF;
		IF l ~ '[1-9]\d* maps built' THEN served := 'new map'; END IF;
		IF l ~ '\d+ heap fetches' THEN heap_fetches := substring(l from '(\d+) heap fetches')::int; END IF;
	END LOOP;
END $$;
CREATE FUNCTION report(stage text) RETURNS TABLE (stage text, kind text, same_answers bool, served text, heap_fetches int) LANGUAGE sql AS $$
	SELECT stage, name, answer(q(expr), true) = answer(q(expr), false), h.served, h.heap_fetches FROM attrs, LATERAL how(q(expr)) h ORDER BY n
$$;

-- the parent has no journal: a map is the state of its pages
SELECT * FROM report('first');
SELECT * FROM report('shared');

-- the child written after the maps were made: nothing of the parent changed
UPDATE ch SET g = (g + 1) % 5, tag = 'w' || id % 3 WHERE id % 7 = 0;
INSERT INTO ch SELECT i, i % 5100, i % 5, i % 101, 'u' || i % 4 FROM generate_series(40001, 41000) i;
SELECT * FROM report('child written');

-- the parent written and not vacuumed, then vacuumed
UPDATE pd SET c2 = (c2 + 1) % 7, c4 = (c4 + 1) % 11, c8 = c8 + 10000000000, cd = cd + 1, cts = cts + interval '1 hour', cb = NOT cb, ct = 'x' || k % 23, cn = cn + 0.01
	WHERE k % 31 = 0;
SELECT * FROM report('parent written');
VACUUM pd;
SELECT * FROM report('parent vacuumed');
SELECT * FROM report('parent vacuumed, shared');

-- the workers read the maps from the leader's shared memory, and the lookup tables they make themselves
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.agg_par_min_rows = 0;
SELECT * FROM report('workers');
RESET max_parallel_workers_per_gather;
SET max_parallel_workers_per_gather = 0;

-- the child connected to the parent's journal (via map: nothing is carried): a write to the parent is a new map without a vacuum
ALTER INDEX ch_col SET (connect = 'k -> pd(k)');
REINDEX INDEX ch_col;
SELECT * FROM report('connected');
SELECT * FROM report('connected, shared');
UPDATE ch SET v = (v + 1) % 101 WHERE id % 11 = 0;
SELECT * FROM report('connected, child written');
UPDATE pd SET c2 = (c2 + 1) % 7, c4 = (c4 + 1) % 11, c8 = c8 + 10000000000, cd = cd + 1, cts = cts + interval '1 hour', cb = NOT cb, ct = 'y' || k % 23, cn = cn + 0.01
	WHERE k % 37 = 0;
SELECT * FROM report('connected, parent written');
SELECT * FROM report('connected, parent written, shared');
INSERT INTO pd SELECT i, 1, 1, 1, date '2024-02-01', timestamp '2024-02-01', true, 'z', 1.5 FROM generate_series(5000, 5049) i;
DELETE FROM pd WHERE k % 53 = 0;
SELECT * FROM report('connected, parent inserted and deleted');
SELECT * FROM report('connected, parent inserted and deleted, shared');

-- the reader's own writes are in the answers and the map it makes is nobody else's
BEGIN;
UPDATE pd SET c2 = (c2 + 2) % 7, c4 = (c4 + 2) % 11, c8 = c8 + 20000000000, cd = cd + 2, cts = cts + interval '2 hours', cb = NOT cb, ct = 'q' || k % 23, cn = cn + 0.02
	WHERE k % 41 = 0;
SELECT * FROM report('own transaction');
ROLLBACK;
SELECT * FROM report('after the rollback');

SET max_parallel_workers_per_gather = 2;
SELECT * FROM report('connected, workers');

DROP FUNCTION report(text), how(text), answer(text, bool), q(text);
DROP TABLE attrs, ch, pd;
DROP EXTENSION pg_sift;
