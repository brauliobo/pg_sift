-- The guards the planners of the aggregation share (src/agg/safety.c) and the bridges of SiftAggV6 (src/agg/): the Top-N tournament, the window functions and DISTINCT ON, the stream of
-- groups, the join filter, the cardinality estimate and the expression machine, each held to PostgreSQL's own answer to the same query over the same rows. The test format reads a table through an
-- index whose name ends in _v6, with its rows sorted by the columns of the index. The data is made by arithmetic, so that the output does not depend on the version.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET timezone = 'UTC';

SELECT sift._test_agg_guards() AS wrong;

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- row-level security and security barriers: the scan reads raw columns, so any policy, barrier or FORCE ROW LEVEL SECURITY makes the query one the normal plan answers
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
CREATE ROLE sift_v6_reader NOLOGIN;
CREATE TABLE rls_plain (id int, tenant int);
CREATE TABLE rls_policy (id int, tenant int);
CREATE TABLE rls_forced (id int, tenant int);
INSERT INTO rls_plain SELECT i, i % 3 FROM generate_series(1, 30) i;
INSERT INTO rls_policy SELECT i, i % 3 FROM generate_series(1, 30) i;
INSERT INTO rls_forced SELECT i, i % 3 FROM generate_series(1, 30) i;
ALTER TABLE rls_policy ENABLE ROW LEVEL SECURITY;
ALTER TABLE rls_forced ENABLE ROW LEVEL SECURITY;
ALTER TABLE rls_forced FORCE ROW LEVEL SECURITY;
CREATE POLICY only_one ON rls_policy USING (tenant = 1);
CREATE POLICY only_two ON rls_forced USING (tenant = 2);
CREATE VIEW rls_plain_view AS SELECT id, tenant FROM rls_plain;
CREATE VIEW rls_barrier_view WITH (security_barrier = true) AS SELECT id, tenant FROM rls_plain WHERE tenant = 1;
CREATE VIEW rls_policy_view AS SELECT id, tenant FROM rls_policy;
GRANT SELECT ON rls_plain, rls_policy, rls_forced, rls_plain_view, rls_barrier_view, rls_policy_view TO sift_v6_reader;

CREATE TABLE rls_cases (name text, query text);
INSERT INTO rls_cases VALUES
    ('table', 'SELECT id FROM rls_plain'),
    ('policy', 'SELECT id FROM rls_policy'),
    ('forced', 'SELECT id FROM rls_forced'),
    ('plain view', 'SELECT id FROM rls_plain_view'),
    ('barrier view', 'SELECT id FROM rls_barrier_view'),
    ('view of a table with a policy', 'SELECT id FROM rls_policy_view'),
    ('CTE of a table with a policy', 'WITH c AS (SELECT id FROM rls_policy) SELECT id FROM c'),
    ('sub-select of a table with a policy', 'SELECT id FROM rls_plain WHERE id IN (SELECT id FROM rls_policy)'),
    ('join of a table with a policy', 'SELECT a.id FROM rls_plain a JOIN rls_policy b ON a.id = b.id'),
    ('sub-select of a plain table', 'SELECT id FROM rls_plain WHERE id IN (SELECT id FROM rls_plain)');
GRANT SELECT ON rls_cases TO sift_v6_reader;

SELECT name, sift._test_agg_query_rls(query) AS owner FROM rls_cases ORDER BY name;
SET ROLE sift_v6_reader;
SELECT name, sift._test_agg_query_rls(query) AS reader FROM rls_cases ORDER BY name;
RESET ROLE;

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- the table of the bridges
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
CREATE TABLE v6t (g int4, a int4, b int8, v float8, ts timestamptz, d date, n numeric(10, 2), f bool, id int8);
INSERT INTO v6t
SELECT CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END, (i * 37) % 100, (i * 7919) % 5000, CASE WHEN i % 17 = 0 THEN NULL ELSE (i % 50) / 4.0 END,
       timestamptz '2026-01-01 00:00:00+00' + i * interval '37 minutes', date '2026-01-01' + i % 200, ((i * 13) % 1000) / 100.0, i % 3 = 0, i
FROM generate_series(1, 3000) i;
CREATE INDEX v6t_idx_v6 ON v6t (g, a, b, v, ts, d, n, f);
CREATE TABLE v6dim (k int4, tag text);
INSERT INTO v6dim SELECT i, CASE WHEN i % 25 = 0 THEN 'hot' ELSE 'cold' END FROM generate_series(0, 99) i;
ANALYZE v6t;
ANALYZE v6dim;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_sort = off;

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- the expression machine: the rows that pass a WHERE clause as the machine evaluates it, held to PostgreSQL's evaluation of it. refused: the machine does not run it
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
CREATE FUNCTION v6_truth(cond text, p int8) RETURNS int8[] LANGUAGE plpgsql AS $$
DECLARE r int8[];
BEGIN
    EXECUTE format('SELECT coalesce(array_agg(id ORDER BY ctid), ''{}'') FROM v6t WHERE %s', cond) INTO r USING p;
    RETURN r;
END $$;

CREATE TABLE v6_conds (cond text, p int8);
INSERT INTO v6_conds VALUES
    ('a = 5', NULL), ('a <> 5', NULL), ('a < 10', NULL), ('a >= 90', NULL), ('b > 4000', NULL), ('b <= 500', NULL), ('b > 100 AND b < 200', NULL), ('a BETWEEN 10 AND 20', NULL),
    ('g IS NULL', NULL), ('g IS NOT NULL AND g > 3', NULL), ('v IS NULL', NULL), ('v > 5.5', NULL), ('v <= 2', NULL), ('v = 0', NULL), ('n > 5', NULL), ('n + 1.5 > 9', NULL),
    ('f', NULL), ('NOT f', NULL), ('f AND a > 50', NULL), ('f OR a < 3', NULL), ('NOT (a < 50 OR f)', NULL), ('ts > ''2026-03-01''::timestamptz', NULL),
    ('d >= ''2026-02-01''::date AND d < ''2026-03-01''::date', NULL), ('a + b > 4500', NULL), ('b - a * 2 < 100', NULL), ('abs(b - 2500) < 50', NULL), ('-a < -95', NULL),
    ('b / 7 = 100', NULL), ('v * 2 > 20', NULL), ('v + a > 40', NULL), ('coalesce(g, -1) = -1', NULL), ('coalesce(v, 0) > 10', NULL),
    ('CASE WHEN a < 10 THEN true WHEN b > 4900 THEN true ELSE false END', NULL), ('CASE WHEN g IS NULL THEN a ELSE b END > 3000', NULL),
    ('date_trunc(''day'', ts) = ''2026-02-01''::timestamptz', NULL), ('date_trunc(''hour'', ts) > ''2026-04-01''::timestamptz', NULL),
    ('extract(hour FROM ts) = 3', NULL), ('extract(year FROM d) = 2026 AND extract(month FROM d) = 3', NULL), ('extract(dow FROM ts) = 0', NULL), ('extract(epoch FROM ts) > 1780000000', NULL),
    ('b > $1', 2500), ('a = $1', 42), ('a = $1', NULL), ('b <= $1 + 10', 100), ('NOT (a > $1)', 50),
    ('a = ANY (ARRAY[1, 2, 3])', NULL), ('a IS NOT DISTINCT FROM 5', NULL), ('id > 2900', NULL), ('b % 7 = 0', NULL), ('a::text = ''5''', NULL), ('n / 3 > 1', NULL);

SELECT cond, p, got IS NULL AS refused, got IS NULL OR got = sift_truth AS same
FROM (SELECT cond, p, sift._test_v6_filter('v6t_idx_v6', cond, p) AS got, v6_truth(cond, p) AS sift_truth FROM v6_conds) AS r
ORDER BY refused, cond, p;

-- a timestamp is read in the zone of the session, the one the plan has when it is made and the one it runs in
SET timezone = 'America/Sao_Paulo';
SELECT cond, sift._test_v6_filter('v6t_idx_v6', cond) = v6_truth(cond, NULL) AS same
FROM (VALUES ('date_trunc(''day'', ts) = ''2026-02-01 00:00:00-03''::timestamptz'), ('extract(hour FROM ts) = 22'), ('date_trunc(''month'', ts) = ''2026-03-01 00:00:00-03''::timestamptz')) AS c(cond);
SET timezone = 'UTC';

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- the bridges in the planner: the plan has the node, and the rows are those of the plan PostgreSQL makes without it
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
CREATE TABLE v6_checks (name text, query text, node text, bridge text, ordered bool);
INSERT INTO v6_checks VALUES
    ('topn: two keys', 'SELECT a, b FROM v6t ORDER BY a, b DESC LIMIT 10', 'SiftAggV6TopN', 'topn', true),
    ('topn: a WHERE clause and an offset', 'SELECT g, a FROM v6t WHERE b > 1000 ORDER BY g DESC NULLS FIRST, a LIMIT 15 OFFSET 5', 'SiftAggV6TopN', 'topn', true),
    ('topn: a float key with NULLs', 'SELECT v FROM v6t ORDER BY v LIMIT 20', 'SiftAggV6TopN', 'topn', true),
    ('topn: a timestamp descending', 'SELECT ts, a FROM v6t ORDER BY ts DESC LIMIT 7', 'SiftAggV6TopN', 'topn', true),
    ('topn: a numeric key', 'SELECT n, b FROM v6t ORDER BY n DESC, b LIMIT 12', 'SiftAggV6TopN', 'topn', true),
    ('topn: WITH TIES', 'SELECT a FROM v6t ORDER BY a FETCH FIRST 5 ROWS WITH TIES', 'SiftAggV6TopN', 'topn', true),
    ('topn: a column that is not in the index', 'SELECT id, a FROM v6t ORDER BY a, b LIMIT 5', 'SiftAggV6TopN', 'topn', true),
    ('topn: a boolean and a date in the clauses', 'SELECT d, a FROM v6t WHERE f AND d > ''2026-03-01''::date ORDER BY d DESC, a LIMIT 9', 'SiftAggV6TopN', 'topn', true),
    ('topn: an expression key stays with PostgreSQL', 'SELECT a FROM v6t ORDER BY a + 1 LIMIT 5', '', 'topn', true),
    ('topn: a clause the machine does not run', 'SELECT a FROM v6t WHERE b % 7 = 0 ORDER BY a LIMIT 5', '', 'topn', true),
    ('window: row_number over unique keys', 'SELECT g, a, b, row_number() OVER (PARTITION BY g ORDER BY a, b) AS rn FROM v6t ORDER BY g, a, b', 'SiftAggV6Window', 'window', true),
    ('window: ranks of ties', 'SELECT g, a, rank() OVER w AS r, dense_rank() OVER w AS dr FROM v6t WINDOW w AS (PARTITION BY g ORDER BY a) ORDER BY g, a', 'SiftAggV6Window', 'window', true),
    ('window: lag and lead', 'SELECT g, a, b, lag(b) OVER w AS prev, lead(b, 2, -1) OVER w AS next FROM v6t WINDOW w AS (PARTITION BY g ORDER BY a, b) ORDER BY g, a, b', 'SiftAggV6Window', 'window', true),
    ('window: running aggregates', 'SELECT g, a, b, sum(b) OVER w AS s, count(*) OVER w AS c, min(a) OVER w AS lo, max(v) OVER w AS hi FROM v6t WINDOW w AS (PARTITION BY g ORDER BY a, b) ORDER BY g, a, b', 'SiftAggV6Window', 'window', true),
    ('window: aggregates over peers', 'SELECT g, a, sum(b) OVER w AS s, avg(a) OVER w AS m, count(v) OVER w AS c FROM v6t WINDOW w AS (PARTITION BY g ORDER BY a) ORDER BY g, a', 'SiftAggV6Window', 'window', true),
    ('window: a WHERE clause', 'SELECT g, a, b, row_number() OVER (PARTITION BY g ORDER BY a, b) AS rn FROM v6t WHERE b < 2500 AND f ORDER BY g, a, b', 'SiftAggV6Window', 'window', true),
    ('window: the first rows of each partition', 'SELECT * FROM (SELECT g, a, b, row_number() OVER (PARTITION BY g ORDER BY a, b) AS rn FROM v6t) AS s WHERE rn <= 3 ORDER BY g, a, b', 'SiftAggV6Window', 'window', true),
    ('window: the first ranks of each partition', 'SELECT * FROM (SELECT g, a, rank() OVER (PARTITION BY g ORDER BY a) AS r FROM v6t) AS s WHERE r < 3 ORDER BY g, a', 'SiftAggV6Window', 'window', true),
    ('window: parts not sorted by the order are sorted by partition', 'SELECT g, a, row_number() OVER (PARTITION BY g ORDER BY b) AS rn FROM v6t ORDER BY g, b', 'SiftAggV6Window', 'window', true),
    ('window: parts not sorted by the key are put together', 'SELECT a, b, sum(b) OVER (PARTITION BY a ORDER BY b) AS s, lag(g) OVER (PARTITION BY a ORDER BY b) AS prev FROM v6t ORDER BY a, b', 'SiftAggV6Window', 'window', true),
    ('window: a partition key that is not a column stays with PostgreSQL', 'SELECT g, a, row_number() OVER (PARTITION BY g + 1 ORDER BY b) AS rn FROM v6t ORDER BY g, b', '', 'window', true),
    ('distinct: DISTINCT ON the first row hashed', 'SELECT DISTINCT ON (g) g, a FROM v6t ORDER BY g, a', 'SiftAggV6Window', 'window', true),
    ('distinct: DISTINCT ON the last row hashed', 'SELECT DISTINCT ON (g) g, a FROM v6t ORDER BY g, a DESC NULLS FIRST', 'SiftAggV6Window', 'window', true),
    ('distinct: DISTINCT ON with a WHERE clause hashed', 'SELECT DISTINCT ON (g, a) g, a FROM v6t WHERE b > 1000 ORDER BY g, a', 'SiftAggV6Window', 'window', true),
    ('stream: one key', 'SELECT g, count(*), count(a), sum(a), min(a), max(a) FROM v6t GROUP BY g ORDER BY g', 'SiftAggV6Stream', 'stream', true),
    ('stream: two keys', 'SELECT g, a, count(*), sum(b) FROM v6t GROUP BY g, a ORDER BY g, a', 'SiftAggV6Stream', 'stream', true),
    ('stream: a WHERE clause and HAVING', 'SELECT g, count(*), max(b) FROM v6t WHERE b < 2000 AND f GROUP BY g HAVING count(*) > 5 ORDER BY g', 'SiftAggV6Stream', 'stream', true),
    ('stream: only count', 'SELECT g, a, count(*) FROM v6t WHERE a > 90 GROUP BY g, a ORDER BY g, a', 'SiftAggV6Stream', 'stream', true),
    ('stream: not a prefix of the index stays with PostgreSQL', 'SELECT a, count(*) FROM v6t GROUP BY a ORDER BY a', '', 'stream', true),
    ('stream: two value columns', 'SELECT g, sum(a), sum(b) FROM v6t GROUP BY g ORDER BY g', 'SiftAggV6Stream', 'stream', true),
    ('sip: a dimension that keeps few keys', 'SELECT count(*), sum(f.b) FROM v6t f JOIN v6dim d ON f.a = d.k WHERE d.tag = ''hot''', 'SiftAggV6Sip', 'sip', false),
    ('sip: a condition on the fact table', 'SELECT f.g, count(*) FROM v6t f JOIN v6dim d ON f.a = d.k WHERE d.tag = ''hot'' AND f.b < 3000 GROUP BY f.g ORDER BY f.g', 'SiftAggV6Sip', 'sip', true),
    ('sip: a dimension that keeps no key', 'SELECT count(*) FROM v6t f JOIN v6dim d ON f.a = d.k WHERE d.tag = ''none''', 'SiftAggV6Sip', 'sip', false),
    ('sip: an outer join stays with PostgreSQL', 'SELECT count(*) FROM v6t f LEFT JOIN v6dim d ON f.a = d.k AND d.tag = ''hot''', '', 'sip', false),
    ('sip: a semi join of the dimension', 'SELECT count(*), sum(f.b) FROM v6t f WHERE f.a IN (SELECT k FROM v6dim WHERE tag = ''hot'')', 'SiftAggV6Sip', 'sip', false),
    ('sip: an outer join of another table', 'SELECT count(*), sum(o.k) FROM v6t f JOIN v6dim d ON f.a = d.k LEFT JOIN v6dim o ON o.k = f.g WHERE d.tag = ''hot''', 'SiftAggV6Sip', 'sip', false),
    ('sip: an anti join stays with PostgreSQL', 'SELECT count(*) FROM v6t f WHERE NOT EXISTS (SELECT 1 FROM v6dim d WHERE d.k = f.a AND d.tag = ''hot'')', '', 'sip', false),
    ('sip: an outer join condition keeps its rows', 'SELECT count(*), count(o.k) FROM v6t f CROSS JOIN v6dim d LEFT JOIN v6dim o ON o.k = d.k AND f.a = d.k WHERE d.tag = ''hot''', '', 'sip', false);

SELECT name, sift._test_v6_check(query, node, ARRAY[bridge], ordered) AS result FROM v6_checks ORDER BY name;

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- under row-level security of the query no bridge is chosen
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
CREATE TABLE v6rls (g int4, a int4, b int8, v float8, ts timestamptz, d date, n numeric(10, 2), f bool, id int8);
INSERT INTO v6rls SELECT g, a, b, v, ts, d, n, f, id FROM v6t WHERE id <= 500;
CREATE INDEX v6rls_idx_v6 ON v6rls (g, a, b, v, ts, d, n, f);
ALTER TABLE v6rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY v6rls_p ON v6rls USING (a < 50);
GRANT SELECT ON v6rls TO sift_v6_reader;
GRANT USAGE ON SCHEMA sift TO sift_v6_reader;
GRANT SELECT ON v6dim TO sift_v6_reader;
SET ROLE sift_v6_reader;
SELECT sift._test_v6_check('SELECT a, b FROM v6rls ORDER BY a, b LIMIT 10', '', ARRAY['topn', 'window', 'stream', 'sip', 'card'], true) AS topn;
SELECT sift._test_v6_check('SELECT g, a, b, row_number() OVER (PARTITION BY g ORDER BY a, b) FROM v6rls ORDER BY g, a, b', '', ARRAY['topn', 'window', 'stream', 'sip', 'card'], true) AS win;
SELECT sift._test_v6_check('SELECT g, count(*) FROM v6rls GROUP BY g ORDER BY g', '', ARRAY['topn', 'window', 'stream', 'sip', 'card'], true) AS stream;
SELECT sift._test_v6_check('SELECT count(*) FROM v6rls f JOIN v6dim d ON f.a = d.k WHERE d.tag = ''hot''', '', ARRAY['topn', 'window', 'stream', 'sip', 'card'], false) AS sip;
RESET ROLE;

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- the cardinality bridge: clauses the statistics answer, the others left to PostgreSQL, a parameter of a generic plan that is none of them, and a relation under row security that is not touched
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
SELECT q, npreds, nrest, replaced, round(rows) AS rows
FROM (VALUES ('SELECT * FROM v6t WHERE a = 5'), ('SELECT * FROM v6t WHERE a < 10'), ('SELECT * FROM v6t WHERE a BETWEEN 10 AND 19'), ('SELECT * FROM v6t WHERE a = ANY (ARRAY[1, 2, 3, 4])'),
             ('SELECT * FROM v6t WHERE g IS NULL'), ('SELECT * FROM v6t WHERE a = 5 AND b > 2500'), ('SELECT * FROM v6t WHERE a = 5 AND f'), ('SELECT * FROM v6t WHERE a + 1 = 5'),
             ('SELECT * FROM v6t WHERE a = 1000'), ('SELECT * FROM v6t')) AS c(q), LATERAL sift._test_v6_card(q) ORDER BY q;

SET plan_cache_mode = force_generic_plan;
PREPARE v6_eq (int4) AS SELECT * FROM v6t WHERE a = $1;
PREPARE v6_out (int4) AS SELECT * FROM v6t WHERE a = $1 AND b > 2500;
PREPARE v6_in (int4[]) AS SELECT * FROM v6t WHERE a = ANY ($1);
SELECT q, npreds, nrest, replaced FROM (VALUES ('EXECUTE v6_eq(1000)'), ('EXECUTE v6_out(1000)'), ('EXECUTE v6_in(ARRAY[1, 2])')) AS c(q), LATERAL sift._test_v6_card(q) ORDER BY q;
RESET plan_cache_mode;

SET ROLE sift_v6_reader;
SELECT q, npreds, nrest, replaced FROM (VALUES ('SELECT * FROM v6rls WHERE a = 5')) AS c(q), LATERAL sift._test_v6_card(q);
RESET ROLE;

DROP FUNCTION v6_truth(text, int8);
DROP TABLE v6_checks, v6_conds, v6t, v6dim, v6rls, rls_cases;
DROP VIEW rls_plain_view, rls_barrier_view, rls_policy_view;
DROP TABLE rls_plain, rls_policy, rls_forced;
DROP OWNED BY sift_v6_reader;
DROP ROLE sift_v6_reader;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
