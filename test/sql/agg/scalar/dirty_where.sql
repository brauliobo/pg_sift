-- A condition of the WHERE that casts a typed path (`doc.v float8`) as `(doc ->> 'v')::float8 > 3`. The scan reads the rows that hold a value the
-- leaf cannot store again from the heap; one passes the condition, goes on to the other conditions and raises the error of the cast only if they
-- keep it. PostgreSQL tests the conditions of a scan by cost, the written order among equals, so that is its answer exactly when the others come
-- first: then a row they drop never raises. Where the cast could run first (a costlier condition, one that raises too) the query keeps the normal
-- plan, and sift.explain_refusals names dirty_order.
-- Every query runs with and without the pushdown; the rows, or the error, must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

\ir test/agg_helpers.sql

CREATE FUNCTION outcome(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a <> b THEN format('DIFF off=%s on=%s', left(a, 200), left(b, 200))
	            WHEN a LIKE 'ERR %' THEN 'same ' || left(a, 70) ELSE 'same rows' END;
END $$;

-- the plan the pushdown gives the query
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE sql AS $$ SELECT CASE WHEN pushed(q) THEN 'scan' ELSE 'normal' END $$;

CREATE FUNCTION reread(q text) RETURNS bigint LANGUAGE sql AS $$ SELECT sift_helpers.agg_heap_rereads(q) $$;

CREATE TABLE w_t (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO w_t
SELECT i, i % 5, jsonb_build_object('v', (i % 17) + 0.5, 'n', i % 9, 'ok', i % 2 = 0, 't', 'k' || (i % 4), 'c', i % 3)
FROM generate_series(1, 3000) i;
CREATE INDEX w_t_col ON w_t USING sift_col (id, grp, doc) WITH (paths = 'doc.v float8, doc.n int4, doc.ok bool, doc.t text, doc.c int4');
-- numeric strings in the paths the conditions cast, none in doc.c
UPDATE w_t SET doc = jsonb_set(doc, '{v}', to_jsonb((id % 17 + 0.5)::text)) WHERE id % 33 = 5;
UPDATE w_t SET doc = jsonb_set(doc, '{n}', to_jsonb((id % 9)::text)) WHERE id % 29 = 5;
UPDATE w_t SET doc = jsonb_set(doc, '{ok}', '"true"') WHERE id % 31 = 5;
VACUUM ANALYZE w_t;

CREATE TABLE w_q (n text PRIMARY KEY, q text);
INSERT INTO w_q VALUES
	('gt',              'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 GROUP BY grp'),
	('gt and other',    'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND grp <> 2 GROUP BY grp'),
	('other first',     'SELECT grp, count(*) FROM w_t WHERE grp <> 2 AND (doc ->> ''v'')::float8 > 3 GROUP BY grp'),
	('and the key',     'SELECT grp, sum((doc ->> ''v'')::float8) FROM w_t WHERE (doc ->> ''v'')::float8 < 9 GROUP BY grp'),
	('range',           'SELECT grp, count(*), avg((doc ->> ''v'')::float8) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND (doc ->> ''v'')::float8 < 12 GROUP BY grp'),
	('cast of jsonb',   'SELECT grp, count(*) FROM w_t WHERE (doc -> ''v'')::float8 > 3 GROUP BY grp'),
	('int4 eq',         'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''n'')::int4 = 3 GROUP BY grp'),
	('in list',         'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''n'')::int4 IN (1, 3, 5) GROUP BY grp'),
	('is null',         'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 IS NULL GROUP BY grp'),
	('is not null',     'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 IS NOT NULL GROUP BY grp'),
	('key and filter',  'SELECT (doc ->> ''n'')::int4 k, count(*) FILTER (WHERE grp > 1) FROM w_t WHERE (doc ->> ''n'')::int4 >= 0 GROUP BY 1'),
	('having',          'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 GROUP BY grp HAVING max((doc ->> ''v'')::float8) > 10'),
	('no groups',       'SELECT count(*), min((doc ->> ''v'')::float8) FROM w_t WHERE (doc ->> ''v'')::float8 > 3'),
	('clean leaf',      'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''c'')::int4 = 1 AND (doc ->> ''v'')::float8 > 3 GROUP BY grp');
SELECT n, plan_of(q) AS plan, reread(q) AS reread, outcome(q) FROM w_q ORDER BY n;

-- the units a condition rules out by their range still hold rows that are read again: a unit whose clean values are all below 3
CREATE TABLE w_z (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO w_z SELECT i, i % 3, jsonb_build_object('v', (i % 3)::float8) FROM generate_series(1, 2000) i;
CREATE INDEX w_z_col ON w_z USING sift_col (id, grp, doc) WITH (paths = 'doc.v float8');
UPDATE w_z SET doc = jsonb_set(doc, '{v}', '"9.5"') WHERE id IN (7, 1500);
VACUUM ANALYZE w_z;
SELECT n, plan_of(q) AS plan, reread(q) AS reread, outcome(q) FROM (VALUES
	('range rules the unit out', 'SELECT grp, count(*) FROM w_z WHERE (doc ->> ''v'')::float8 > 3 GROUP BY grp'),
	('and below',                'SELECT count(*) FROM w_z WHERE (doc ->> ''v'')::float8 < 3')) v(n, q) ORDER BY n;
DROP TABLE w_z;

-- a value SQL cannot convert: the row raises where the others keep it, as the normal plan does
UPDATE w_t SET doc = jsonb_set(doc, '{v}', '"abc"') WHERE id = 1234;
UPDATE w_t SET doc = jsonb_set(doc, '{n}', '3.5') WHERE id = 1236;
UPDATE w_t SET doc = jsonb_set(doc, '{ok}', '"maybe"') WHERE id = 1238;
VACUUM ANALYZE w_t;
TRUNCATE w_q;
INSERT INTO w_q VALUES
	('raises',               'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 GROUP BY grp'),
	('cast of jsonb raises', 'SELECT grp, count(*) FROM w_t WHERE (doc -> ''v'')::float8 > 3 GROUP BY grp'),
	('int4 raises',          'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''n'')::int4 = 3 GROUP BY grp'),
	('in list raises',       'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''n'')::int4 IN (1, 3, 5) GROUP BY grp'),
	('is null raises',       'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 IS NULL GROUP BY grp'),
	('raises, in a range',   'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND (doc ->> ''v'')::float8 < 12 GROUP BY grp'),
	-- the row is dropped by a cheaper condition before the cast: nothing is raised
	('dropped by id',        'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND id <> 1234 GROUP BY grp'),
	('dropped by grp',       'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND grp <> 4 GROUP BY grp'),
	('dropped, written 1st', 'SELECT grp, count(*) FROM w_t WHERE id <> 1234 AND (doc ->> ''v'')::float8 > 3 GROUP BY grp'),
	-- the same cost: PostgreSQL tests them in the order they are written
	('same cost, c first',   'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''c'')::int4 <> 1 AND (doc ->> ''v'')::float8 > 3 GROUP BY grp'),
	('same cost, c after',   'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND (doc ->> ''c'')::int4 <> 1 GROUP BY grp'),
	('dropped, int4',        'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''n'')::int4 = 3 AND id <> 1236 GROUP BY grp'),
	('kept by the rest',     'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND grp = 4 GROUP BY grp'),
	('the other leaf only',  'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''n'')::int4 = 3 GROUP BY grp'),
	('a clean leaf',         'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''c'')::int4 = 1 GROUP BY grp');
SELECT n, plan_of(q) AS plan, outcome(q) FROM w_q ORDER BY n;

-- the cast could run before a condition that drops the row, or a condition raises too: the normal plan, rows or error as the heap
TRUNCATE w_q;
INSERT INTO w_q VALUES
	-- a costlier condition on the same row (a list of 8 strings: from 9 PostgreSQL hashes the list, which costs less) is tested after the cast
	('costlier condition', 'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND doc ->> ''t'' IN (''k1'', ''k2'', ''k3'', ''k1'', ''k2'', ''k3'', ''k1'', ''x'') GROUP BY grp'),
	('may raise too',      'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND 100 / (grp + 1) > 3 GROUP BY grp'),
	-- two leaves with bad values: each is tested before the other, whichever comes first
	('two casts',          'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND (doc ->> ''n'')::int4 > 1 GROUP BY grp'),
	-- a condition of another shape than a comparison, an IN list of integers or a NULL test: not read again yet
	('expression',         'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''n'')::int4 % 2 = 1 GROUP BY grp'),
	('bool',               'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''ok'')::bool GROUP BY grp'),
	('in list of floats',  'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 IN (1.5, 2.5) GROUP BY grp'),
	('read in a case',     'SELECT grp, sum(CASE WHEN grp = 1 THEN (doc ->> ''v'')::float8 END) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 GROUP BY grp'),
	('or',                 'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 OR grp = 1 GROUP BY grp'),
	('against a leaf',     'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > (doc ->> ''c'')::float8 GROUP BY grp'),
	('sub-select',         'SELECT grp, count(*) FROM w_t WHERE id IN (SELECT id FROM w_t WHERE (doc ->> ''v'')::float8 > 3) GROUP BY grp'),
	('two kinds',          'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 AND (doc -> ''v'')::float8 > 3 GROUP BY grp');
SELECT n, plan_of(q) AS plan, outcome(q) FROM w_q ORDER BY n;

-- the refusals are named
SET sift.enable_agg = on;
SET sift.explain_refusals = on;
EXPLAIN (COSTS OFF) SELECT grp, count(*) FROM w_t WHERE (doc ->> 'v')::float8 > 3 AND 100 / (grp + 1) > 3 GROUP BY grp;
EXPLAIN (COSTS OFF) SELECT grp, count(*) FROM w_t WHERE (doc ->> 'v')::float8 > 3 AND (doc ->> 'n')::int4 > 1 GROUP BY grp;
EXPLAIN (COSTS OFF) SELECT grp, count(*) FROM w_t WHERE (doc ->> 'v')::float8 > 3 OR grp = 1 GROUP BY grp;
EXPLAIN (COSTS OFF) SELECT grp, count(*) FROM w_t WHERE (doc ->> 'ok')::bool GROUP BY grp;
RESET sift.explain_refusals;

-- rows the snapshot does not see raise nothing: the dirty rows are deleted, their units still hold them
DELETE FROM w_t WHERE id IN (1234, 1236, 1238);
SELECT n, plan_of(q) AS plan, outcome(q) FROM (VALUES
	('v deleted',   'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''v'')::float8 > 3 GROUP BY grp'),
	('n deleted',   'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''n'')::int4 = 3 GROUP BY grp'),
	('ok deleted',  'SELECT grp, count(*) FROM w_t WHERE (doc ->> ''ok'')::bool GROUP BY grp')) v(n, q) ORDER BY n;
DROP TABLE w_q;
DROP TABLE w_t;
DROP FUNCTION reread(text);
DROP FUNCTION plan_of(text);
DROP FUNCTION pushed(text);
DROP FUNCTION outcome(text);
DROP FUNCTION runq(text, bool), outcome, runs_in_scan;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
