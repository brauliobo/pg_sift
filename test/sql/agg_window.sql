-- SiftAgg under a window function: PostgreSQL plans the window above the grouping, so the grouping stage is the scan's whether the
-- query has a window function or not. Every query runs with and without the pushdown; the results must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE wn (id int, g int, k text, d date, n int, v numeric(10,2), c int);
INSERT INTO wn SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE 'k' || i % 5 END, date '2024-01-01' + i % 400, i % 100, (i % 977) / 7.0,
	CASE WHEN i % 13 = 0 THEN NULL ELSE i % 300 END FROM generate_series(1, 20000) i;
CREATE INDEX wn_col ON wn USING sift_col (id, g, k, d, n, v, c);
VACUUM ANALYZE wn;

CREATE FUNCTION agg_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION agg_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE agg_q (q text);
INSERT INTO agg_q VALUES
	('SELECT g, count(*), rank() OVER (ORDER BY count(*) DESC, g) FROM wn GROUP BY g'),
	('SELECT k, sum(v), sum(sum(v)) OVER (ORDER BY k NULLS FIRST) FROM wn GROUP BY k'),
	('SELECT date_trunc(''month'', d) m, sum(v), lag(sum(v)) OVER (ORDER BY date_trunc(''month'', d)) FROM wn GROUP BY date_trunc(''month'', d)'),
	('SELECT date_trunc(''month'', d) m, count(*), count(*) - lag(count(*), 1, 0::bigint) OVER (ORDER BY date_trunc(''month'', d)) FROM wn WHERE n > 20 GROUP BY date_trunc(''month'', d) HAVING count(*) > 100'),
	('SELECT g, count(*), round(100.0 * count(*) / sum(count(*)) OVER (), 3) FROM wn GROUP BY g'),
	('SELECT g, k, count(*), row_number() OVER (PARTITION BY g ORDER BY count(*) DESC, k NULLS FIRST) FROM wn GROUP BY g, k'),
	('SELECT * FROM (SELECT g, k, count(*) n, dense_rank() OVER (PARTITION BY g ORDER BY count(*) DESC) r FROM wn GROUP BY g, k) t WHERE r <= 2'),
	('SELECT count(*), sum(v), count(*) OVER (), sum(count(*)) OVER () FROM wn'),
	('SELECT count(*), sum(count(*)) OVER () FROM wn WHERE id < 0'),
	('SELECT g, count(DISTINCT c), ntile(3) OVER (ORDER BY count(DISTINCT c), g) FROM wn GROUP BY g'),
	('SELECT g, sum(n), sum(sum(n)) OVER (ORDER BY g ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM wn GROUP BY g'),
	('SELECT g, avg(n), percent_rank() OVER (ORDER BY avg(n)) FROM wn GROUP BY g'),
	('SELECT g, min(d), first_value(min(d)) OVER (ORDER BY g DESC) FROM wn GROUP BY g'),
	('SELECT k, count(*) FILTER (WHERE n > 50), sum(count(*) FILTER (WHERE n > 50)) OVER () FROM wn GROUP BY k'),
	('SELECT g, count(*), sum(g) OVER () FROM wn GROUP BY g'),
	('SELECT g, k, count(*), grouping(g, k), rank() OVER (ORDER BY count(*) DESC, g, k) FROM wn GROUP BY ROLLUP (g, k)');

SELECT query, pushed, same FROM (SELECT q AS query, agg_pushed(q) AS pushed, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM agg_q) z ORDER BY query COLLATE "C";

-- the window stays above the scan, and a query without a window keeps its plan
SET sift.enable_agg = on;
EXPLAIN (COSTS OFF) SELECT g, count(*), rank() OVER (ORDER BY count(*) DESC, g) FROM wn GROUP BY g;
EXPLAIN (COSTS OFF) SELECT g, count(*) FROM wn GROUP BY g;

DROP TABLE wn, agg_q;
DROP FUNCTION agg_rows, agg_pushed;
DROP EXTENSION pg_sift;
