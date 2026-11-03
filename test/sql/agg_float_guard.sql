-- sum() and avg() of float8 raise PostgreSQL's overflow error where a plain double sum would hide it as Infinity, and not when an input is itself infinite or NaN;
-- the guard reads the selected rows through the kernel of the largest absolute value. Rows a FILTER or a NULL hides do not count. Every query runs as the scan and as
-- the normal plan (outcome: the rows, or the error), and the answers must be the same; `scan` says the plan is the scan's.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;

-- g1 holds finite large values that overflow their sum (1e308 twice), g2 an Infinity, g3 a NaN, g4 small values; hid marks the rows a FILTER hides
CREATE TABLE fg (id int, g int, k text, x float8, hid bool) WITH (autovacuum_enabled = false);
INSERT INTO fg
SELECT i, g, 'k' || g,
       CASE WHEN g = 1 AND i % 50 < 2 THEN 1e308 WHEN g = 2 AND i % 50 = 0 THEN 'Infinity'::float8 WHEN g = 3 AND i % 50 = 0 THEN 'NaN'::float8
            WHEN i % 41 = 0 THEN NULL ELSE (i % 1000)::float8 / 7 END,
       i % 50 < 2
FROM (SELECT i, 1 + i % 4 AS g FROM generate_series(1, 20000) i) s;
CREATE INDEX fg_col ON fg USING sift_col (id, g, k, x, hid);
VACUUM (FREEZE, ANALYZE) fg;

\ir test/agg_helpers.sql

SELECT w, pushed(q) AS scan, left(outcome(q, true), 60) AS answer, outcome(q, false) = outcome(q, true) AS same
FROM (SELECT w, format('SELECT %s FROM fg WHERE %s', sel, cond) AS q
	  FROM (VALUES
		('sum, one group, overflow',          'sum(x)', 'g = 1'),
		('sum, one group, hidden by WHERE',   'sum(x)', 'g = 1 AND NOT hid'),
		('avg, one group, overflow',          'avg(x)', 'g = 1'),
		('sum, infinity',                     'sum(x)', 'g = 2'),
		('sum, nan',                          'sum(x)', 'g = 3'),
		('avg, nan and infinity',             'avg(x)', 'g = 2 OR g = 3'),
		('sum, small values',                 'sum(x)', 'g = 4'),
		('avg, small values',                 'avg(x)', 'g = 4'),
		('sum FILTER hides the overflow',     'sum(x) FILTER (WHERE NOT hid)', 'g = 1'),
		('sum by key, overflow in one',       'k, sum(x)', 'true GROUP BY k'),
		('sum by key, overflow hidden',       'k, sum(x)', 'NOT hid GROUP BY k'),
		('avg by key',                        'k, avg(x)', 'g <> 1 AND g <> 3 GROUP BY k'),
		('sum by key and id range',           'id / 5000, sum(x)', 'g = 4 GROUP BY 1')
	  ) v(w, sel, cond)) t;

DROP FUNCTION outcome, runq, runs_in_scan, pushed;
DROP TABLE fg;
DROP EXTENSION pg_sift;
