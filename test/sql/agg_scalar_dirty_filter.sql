-- The FILTER of an aggregate that casts a typed scalar path (`count(*) FILTER (WHERE (doc ->> 'n')::int4 = 3)`) is tested for every row that reaches the
-- aggregate, so a row whose value the leaf cannot store is read again from the heap and raises where the aggregate is reached, as the normal plan
-- does: after the WHERE and not when a condition drops the row. A cast in the argument of an aggregate with a FILTER runs for the rows the FILTER
-- keeps only, and the row raises there only.
-- Every query runs with and without the pushdown; the rows, or the error, must be the same.
CREATE EXTENSION pg_sift;
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

CREATE FUNCTION reread(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; n bigint := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Rows Read Again From The Heap' THEN n := regexp_replace(l, '\D', '', 'g')::bigint; END IF;
	END LOOP;
	RETURN n;
EXCEPTION WHEN others THEN RETURN -1;
END $$;

CREATE TABLE f_t (id int PRIMARY KEY, grp int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO f_t
SELECT i, i % 5, jsonb_build_object('v', (i % 17) + 0.5, 'n', i % 9, 'c', i % 3)
FROM generate_series(1, 3000) i;
CREATE INDEX f_t_col ON f_t USING sift_col (id, grp, doc) WITH (paths = 'doc.v float8, doc.n int4, doc.c int4');
-- numeric strings in the paths the filters cast, none in doc.c
UPDATE f_t SET doc = jsonb_set(doc, '{v}', to_jsonb((id % 17 + 0.5)::text)) WHERE id % 33 = 5;
UPDATE f_t SET doc = jsonb_set(doc, '{n}', to_jsonb((id % 9)::text)) WHERE id % 29 = 5;
VACUUM ANALYZE f_t;

CREATE TABLE f_q (n text PRIMARY KEY, q text);
INSERT INTO f_q VALUES
	('eq',              'SELECT grp, count(*) FILTER (WHERE (doc ->> ''n'')::int4 = 3) AS c FROM f_t GROUP BY grp'),
	('gt, sum',         'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS c, sum((doc ->> ''v'')::float8) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS s FROM f_t GROUP BY grp'),
	('cast of jsonb',   'SELECT grp, count(*) FILTER (WHERE (doc -> ''v'')::float8 > 3) AS c FROM f_t GROUP BY grp'),
	('two filters',     'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS hi, count(*) FILTER (WHERE (doc ->> ''v'')::float8 < 3) AS lo FROM f_t GROUP BY grp'),
	('in list',         'SELECT grp, count(*) FILTER (WHERE (doc ->> ''n'')::int4 IN (1, 3, 5)) AS c FROM f_t GROUP BY grp'),
	('is not null',     'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 IS NOT NULL) AS c FROM f_t GROUP BY grp'),
	('and a key',       'SELECT (doc ->> ''n'')::int4 k, count(*) FILTER (WHERE grp > 1) AS c FROM f_t GROUP BY 1'),
	('with a where',    'SELECT grp, count(*) FILTER (WHERE (doc ->> ''n'')::int4 = 3) AS c FROM f_t WHERE grp <> 2 GROUP BY grp'),
	('no groups',       'SELECT count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS c FROM f_t'),
	('another read',    'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS c, max((doc ->> ''v'')::float8) AS m FROM f_t GROUP BY grp'),
	('a clean leaf',    'SELECT grp, count(*) FILTER (WHERE (doc ->> ''c'')::int4 = 1) AS c FROM f_t GROUP BY grp');
SELECT n, plan_of(q) AS plan, reread(q) AS reread, outcome(q) FROM f_q ORDER BY n;

-- a value SQL cannot convert: the row raises where it reaches the aggregate, unless a condition of the WHERE drops it
UPDATE f_t SET doc = jsonb_set(doc, '{v}', '"abc"') WHERE id = 1234;
UPDATE f_t SET doc = jsonb_set(doc, '{n}', '3.5') WHERE id = 1236;
VACUUM ANALYZE f_t;
TRUNCATE f_q;
INSERT INTO f_q VALUES
	('raises',            'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS c FROM f_t GROUP BY grp'),
	('int4 raises',       'SELECT grp, count(*) FILTER (WHERE (doc ->> ''n'')::int4 = 3) AS c FROM f_t GROUP BY grp'),
	('cast raises',       'SELECT grp, count(*) FILTER (WHERE (doc -> ''v'')::float8 > 3) AS c FROM f_t GROUP BY grp'),
	('raises, two',       'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS hi, count(*) FILTER (WHERE grp = 1) AS g FROM f_t GROUP BY grp'),
	('dropped by id',     'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS c FROM f_t WHERE id <> 1234 GROUP BY grp'),
	('dropped by grp',    'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS c FROM f_t WHERE grp <> 4 GROUP BY grp'),
	('dropped, int4',     'SELECT grp, count(*) FILTER (WHERE (doc ->> ''n'')::int4 = 3) AS c FROM f_t WHERE id <> 1236 GROUP BY grp'),
	('kept by the where', 'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS c FROM f_t WHERE grp = 4 GROUP BY grp'),
	('the other leaf',    'SELECT grp, count(*) FILTER (WHERE (doc ->> ''n'')::int4 = 3) AS c FROM f_t WHERE grp <> 3 GROUP BY grp'),
	('a clean leaf',      'SELECT grp, count(*) FILTER (WHERE (doc ->> ''c'')::int4 = 1) AS c FROM f_t GROUP BY grp');
SELECT n, plan_of(q) AS plan, outcome(q) FROM f_q ORDER BY n;

-- the cast in the argument of an aggregate runs for the rows its FILTER keeps: a row raises there only (a numeric string is converted), and no row does
-- for a FILTER that drops it
TRUNCATE f_q;
INSERT INTO f_q VALUES
	('argument, filtered',  'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 1) AS s FROM f_t GROUP BY grp'),
	('argument, kept',      'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 4) AS s FROM f_t GROUP BY grp'),
	('argument, dropped',   'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE id <> 1234) AS s FROM f_t GROUP BY grp'),
	('argument, int4',      'SELECT grp, sum((doc ->> ''n'')::int4) FILTER (WHERE grp <> 3) AS s FROM f_t GROUP BY grp'),
	('two arguments',       'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 1) AS a, avg((doc ->> ''v'')::float8) FILTER (WHERE grp = 2) AS b FROM f_t GROUP BY grp'),
	('two, one keeps',      'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 1) AS a, avg((doc ->> ''v'')::float8) FILTER (WHERE grp = 4) AS b FROM f_t GROUP BY grp'),
	('and one that does not read', 'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 1) AS a, count(*) AS n FROM f_t GROUP BY grp'),
	('filter of a leaf',    'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE (doc ->> ''c'')::int4 = 1) AS s FROM f_t GROUP BY grp'),
	('in a where',         'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 4) AS s FROM f_t WHERE id <> 1234 GROUP BY grp'),
	('no groups',          'SELECT sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 1) AS s FROM f_t'),
	-- another aggregate or a FILTER casts it for every row
	('and an unfiltered',   'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 1) AS a, max((doc ->> ''v'')::float8) AS b FROM f_t GROUP BY grp'),
	('argument and filter', 'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS s FROM f_t GROUP BY grp'),
	-- the FILTER is no plain test of the cast, or may raise: the normal plan
	('and in the filter',  'SELECT grp, count(*) FILTER (WHERE grp > 1 AND (doc ->> ''v'')::float8 > 3) AS c FROM f_t GROUP BY grp'),
	('or in the filter',   'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3 OR grp = 1) AS c FROM f_t GROUP BY grp'),
	('filter may raise',   'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE 100 / (grp + 1) > 3) AS s FROM f_t GROUP BY grp'),
	('filter of a bad leaf', 'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE (doc ->> ''n'')::int4 = 3) AS s FROM f_t GROUP BY grp'),
	('case in the argument', 'SELECT grp, sum(CASE WHEN grp = 1 THEN (doc ->> ''v'')::float8 END) FILTER (WHERE grp > 0) AS s FROM f_t GROUP BY grp');
SELECT n, plan_of(q) AS plan, outcome(q) FROM f_q ORDER BY n;

SET sift.enable_agg = on;
SET sift.explain_refusals = on;
EXPLAIN (COSTS OFF) SELECT grp, count(*) FILTER (WHERE grp > 1 AND (doc ->> 'v')::float8 > 3) AS c FROM f_t GROUP BY grp;
RESET sift.explain_refusals;

-- rows the snapshot does not see raise nothing
DELETE FROM f_t WHERE id IN (1234, 1236);
SELECT n, plan_of(q) AS plan, outcome(q) FROM (VALUES
	('v deleted', 'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) AS c FROM f_t GROUP BY grp'),
	('n deleted', 'SELECT grp, count(*) FILTER (WHERE (doc ->> ''n'')::int4 = 3) AS c FROM f_t GROUP BY grp')) v(n, q) ORDER BY n;
-- the workers of a parallel scan read the rows again and raise or not as the leader does: the gate of the aggregates travels with the plan
CREATE TABLE f_p (id int PRIMARY KEY, grp int, doc jsonb) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO f_p SELECT i, i % 5, jsonb_build_object('v', (i % 17) + 0.5, 'c', i % 3) FROM generate_series(1, 20000) i;
CREATE INDEX f_p_col ON f_p USING sift_col (id, grp, doc) WITH (paths = 'doc.v float8, doc.c int4');
UPDATE f_p SET doc = jsonb_set(doc, '{v}', to_jsonb((id % 17 + 0.5)::text)) WHERE id % 33 = 5;
UPDATE f_p SET doc = jsonb_set(doc, '{v}', '"abc"') WHERE id = 15001;
VACUUM ANALYZE f_p;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET seq_page_cost = 10000;
CREATE FUNCTION workers(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Workers Planned' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SELECT n, workers(q) AS parallel, outcome(q) FROM (VALUES
	('where, dropped',    'SELECT grp, count(*) FROM f_p WHERE (doc ->> ''v'')::float8 > 3 AND id <> 15001 GROUP BY grp'),
	('where, raises',     'SELECT grp, count(*) FROM f_p WHERE (doc ->> ''v'')::float8 > 3 GROUP BY grp'),
	('filter, raises',    'SELECT grp, count(*) FILTER (WHERE (doc ->> ''v'')::float8 > 3) FROM f_p GROUP BY grp'),
	('gated, raises',     'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 1) FROM f_p GROUP BY grp'),
	('gated, dropped',    'SELECT grp, sum((doc ->> ''v'')::float8) FILTER (WHERE grp = 0) FROM f_p GROUP BY grp')) v(n, q) ORDER BY n;
DROP FUNCTION workers(text);
DROP TABLE f_p;
DROP TABLE f_q;
DROP TABLE f_t;
DROP FUNCTION reread(text);
DROP FUNCTION plan_of(text);
DROP FUNCTION pushed(text);
DROP FUNCTION outcome(text);
DROP FUNCTION runq(text, bool), outcome, runs_in_scan;
DROP EXTENSION pg_sift;
