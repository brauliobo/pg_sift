-- The Dimension Cache of SiftAggV6 (docs/ARCHITECTURE.md): the table of a joined dimension whose journal vouches for it (ev_col connects to dim) is built by the first
-- statement and shared by the next ones, kept in the backend under sift.backend_cache_kb. A write the statement's snapshot sees (the reader's own, or committed since the
-- stamp) makes the next statement build it again, and a budget too small for it, or none, keeps nothing. dim2 has no journal: its table is never kept (shapes 6 and 7).
-- Whatever happens to the tables, the answers are those of the normal plan, the rows of keys the table has no row for (orphans, NULL keys) included.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;

-- 5000 keys in a few classes; the events of keys 5000 to 5099 have no row
CREATE TABLE dim (k int PRIMARY KEY, a int, b int, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 7, i % 3, i % 5 = 0 FROM generate_series(0, 4999) i;
CREATE INDEX dim_col ON dim USING sift_col (k, a, b, flag);
CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 211 = 0 THEN NULL ELSE i % 5100 END, i % 11 FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v) WITH (connect = 'k -> dim(k)');
-- 2,820 classes of two columns: a chunk of the scan meets the rare ones for many chunks, and the aggregates that filter on the class resolve the classes they have not seen alone
CREATE TABLE dim2 (k int PRIMARY KEY, x int, y int) WITH (autovacuum_enabled = false);
INSERT INTO dim2 SELECT i, i % 60, i % 47 FROM generate_series(0, 4999) i;
CREATE INDEX dim2_col ON dim2 USING sift_col (k, x, y);
VACUUM (FREEZE, ANALYZE) ev, dim, dim2;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dim.a, count(*), sum(ev.v) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT dim.a, dim.b, count(*) FROM ev LEFT JOIN dim ON dim.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (3, $q$SELECT ev.id % 4, sum(dim.a), count(dim.flag), max(dim.b), count(DISTINCT ev.k) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (4, $q$SELECT dim.a, count(DISTINCT dim.k), sum(dim.k) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (5, $q$SELECT dim.a, count(DISTINCT dim.k), sum(dim.k) FROM ev LEFT JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (6, $q$SELECT sum(dim2.x), sum(dim2.y), sum(dim2.x * dim2.y) FROM ev JOIN dim2 ON dim2.k = ev.k$q$),
 (7, $q$SELECT dim2.x, count(*) FILTER (WHERE dim2.y < 9) FROM ev LEFT JOIN dim2 ON dim2.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (8, $q$SELECT ev.id % 4, count(*) FILTER (WHERE dim.a = 2), count(*) FILTER (WHERE dim.flag), count(DISTINCT ev.k) FILTER (WHERE dim.b = 1)
		FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$);

-- the maps the statement shared and the ones it made, from EXPLAIN ANALYZE
CREATE FUNCTION maps(sql text, OUT shared int, OUT built int) LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	shared := 0; built := 0;
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ 'Dimension Cache: (\d+) hit' THEN shared := substring(l from 'Dimension Cache: (\d+) hit')::int; END IF;
		IF l ~ 'Dimension Cache: \d+ hit, (\d+) built' THEN built := substring(l from 'Dimension Cache: \d+ hit, (\d+) built')::int; END IF;
	END LOOP;
END $$;
CREATE FUNCTION report(stage text) RETURNS TABLE (stage text, shape int, same_answers bool, shared bool, built bool) LANGUAGE sql AS $$
	SELECT stage, n, sift_helpers.answer(sql, true) = sift_helpers.answer(sql, false), m.shared > 0, m.built > 0 FROM shapes, LATERAL maps(sql) m ORDER BY n
$$;

-- the first statement of a shape builds the table and keeps it; the next ones share it
SELECT * FROM report('first');
SELECT * FROM report('shared');

-- a write, in the reader's own transaction: the table is built again, and the answers see the change
BEGIN;
UPDATE dim SET a = 6, flag = NULL WHERE k % 31 = 0;
DELETE FROM dim WHERE k % 53 = 0;
INSERT INTO dim SELECT i, 1, 1, true FROM generate_series(5000, 5049) i;
SELECT * FROM report('in the transaction');
ROLLBACK;

-- committed, not vacuumed: pages that are not all-visible
UPDATE dim SET b = 2 WHERE k % 29 = 0;
DELETE FROM dim WHERE k % 61 = 0;
SELECT * FROM report('committed, not vacuumed');
VACUUM dim;
SELECT * FROM report('vacuumed');
SELECT * FROM report('shared again');

-- a budget too small for the table of a new state of the dimension, then none: every statement builds it, and keeps nothing
UPDATE dim SET a = 5 WHERE k % 67 = 0;
VACUUM dim;
SET sift.backend_cache_kb = 5;
SELECT * FROM report('budget too small');
SET sift.backend_cache_kb = 0;
SELECT * FROM report('budget off');
RESET sift.backend_cache_kb;
SELECT * FROM report('map made');
SELECT * FROM report('map shared');

-- workers
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SELECT * FROM report('workers');

DROP FUNCTION report(text), maps(text);
DROP TABLE shapes, ev, dim, dim2;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
