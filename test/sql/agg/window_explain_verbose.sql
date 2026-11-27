-- EXPLAIN and EXPLAIN VERBOSE of every kind of SiftAggV6Window plan: the partitions read sorted by the keys and the order, sorted by the keys, put together in memory, DISTINCT ON,
-- and a Sort, a projection or an outer query over the window output. PostgreSQL 18 prints a window function only under its WindowAgg; the scan prints it as the call.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;

CREATE TABLE we (id int, grp int, ts int, v int, n numeric(8,2)) WITH (autovacuum_enabled = false);
INSERT INTO we SELECT i, (i * 7919) % 13, (i * 31) % 3001, CASE WHEN i % 9 = 0 THEN NULL ELSE i % 50 END, (i % 211) / 4.0 FROM generate_series(1, 3000) i;
CREATE TABLE wp (LIKE we) WITH (autovacuum_enabled = false);
CREATE TABLE wg (LIKE we) WITH (autovacuum_enabled = false);
INSERT INTO wp SELECT * FROM we;
INSERT INTO wg SELECT * FROM we;
CREATE INDEX we_col ON we USING sift_col (grp, ts, id, v, n) WITH (key_order = 'grp, ts');
CREATE INDEX wp_col ON wp USING sift_col (grp, ts, id, v, n) WITH (key_order = 'grp');
CREATE INDEX wg_col ON wg USING sift_col (id, grp, ts, v, n) WITH (key_order = 'id');
VACUUM ANALYZE we, wp, wg;

\ir test/common/sql/agg/rows.sql

-- the plan without the line that names a window, which PostgreSQL 18 prints and 17 does not
CREATE FUNCTION win_plan(q text, wide bool) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE format('EXPLAIN (VERBOSE %s, COSTS OFF) %s', wide::text, q) LOOP
		IF l NOT LIKE '%Window: %' THEN RETURN NEXT l; END IF;
	END LOOP;
END $$;

-- the window node is in the plan, both EXPLAINs print, and the rows are those of the normal plan
SELECT q AS query, (SELECT bool_or(l LIKE '%SiftAggV6Window%') FROM win_plan(q, false) l) AS window_node,
	(SELECT count(*) > 0 FROM win_plan(q, true)) AS verbose, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same
FROM (VALUES ('SELECT grp, id, row_number() OVER (PARTITION BY grp ORDER BY ts) FROM we'),
	('SELECT grp, id, rank() OVER (PARTITION BY grp ORDER BY ts DESC, id) FROM wp'),
	('SELECT grp, id, dense_rank() OVER (PARTITION BY grp ORDER BY ts) FROM wg'),
	('SELECT grp, id, row_number() OVER (PARTITION BY grp ORDER BY ts DESC, id) rn FROM wp ORDER BY rn, id'),
	('SELECT * FROM (SELECT grp, id, row_number() OVER (PARTITION BY grp ORDER BY ts DESC, id) rn FROM wp) s WHERE rn <= 3 ORDER BY grp, rn'),
	('SELECT grp, id, row_number() OVER (PARTITION BY grp ORDER BY ts) + 1 FROM we'),
	('SELECT grp, id, count(*) OVER (PARTITION BY grp ORDER BY ts), sum(v) FILTER (WHERE v > 10) OVER (PARTITION BY grp ORDER BY ts) FROM we'),
	('SELECT grp, id, sum(n) OVER (PARTITION BY grp ORDER BY ts DESC, id) FROM wp'),
	('SELECT max(r), count(*) FROM (SELECT rank() OVER (PARTITION BY grp ORDER BY ts) r FROM wg) s')) z(q) ORDER BY 1;

-- DISTINCT ON: the window node on Format 2, SiftAggV6First over the index-only scan on Format 1
SELECT q AS query, (SELECT count(*) > 0 FROM win_plan(q, true)) AS verbose, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same
FROM (VALUES ('SELECT DISTINCT ON (grp) grp, id, ts FROM we ORDER BY grp, ts'), ('SELECT DISTINCT ON (grp) grp, id, ts FROM wg ORDER BY grp, ts DESC')) z(q) ORDER BY 1;

-- what the scan prints: the function calls in its output, and a Sort keyed by one
SELECT * FROM win_plan('SELECT grp, id, row_number() OVER (PARTITION BY grp ORDER BY ts DESC, id) rn FROM wp ORDER BY rn, id', true);
SELECT * FROM win_plan('SELECT grp, id, count(*) OVER (PARTITION BY grp ORDER BY ts), sum(v) FILTER (WHERE v > 10) OVER (PARTITION BY grp ORDER BY ts) FROM we', true);

DROP TABLE we, wp, wg;
DROP FUNCTION agg_rows, agg_pushed, win_plan;
DROP EXTENSION pg_sift;
