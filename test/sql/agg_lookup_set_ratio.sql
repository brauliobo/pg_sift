-- A key list joined twice (a CTE of ids, and the same ids again in an IN sub-select) restricts one column once: the normal plan reads the rows of those keys
-- once, and is priced for them once. Priced twice (the ratio of lookup keys to the column's distinct values, squared), the scan won against a plan 100 times
-- cheaper, and loaded the whole wide table of a flag it tests for a handful of rows. Every statement runs with sift.enable_agg on and off: the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 100;

CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, unit_id int, day date, price float8, latest bool) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, 1 + (i::bigint * 7919) % 20000, 1 + (i::bigint * 104729) % 20000, date '2024-01-01' + (i * 13) % 400, 50 + (i * 31) % 300, i % 4 <> 0
FROM generate_series(1, 300000) i;
CREATE INDEX ev_entity_day ON ev (dim_a_id, day);
CREATE INDEX ev_col ON ev USING sift_col (id, dim_a_id, unit_id, day, price, latest);
VACUUM (FREEZE, ANALYZE) ev;
-- a wide table: its flag is read for the rows of a few entities, and only a scan that loads all of it pays for the rest
CREATE TABLE units (id int PRIMARY KEY, flag bool, pad text) WITH (autovacuum_enabled = false);
INSERT INTO units SELECT i, i % 9 = 0, repeat(md5(i::text), 56) FROM generate_series(1, 20000) i;
VACUUM (FREEZE, ANALYZE) units;
CREATE TABLE scope (id int);
INSERT INTO scope VALUES (11), (22), (33), (44), (55), (66);
CREATE TABLE scope_all AS SELECT g AS id FROM generate_series(1, 20000) g;
ANALYZE scope; ANALYZE scope_all;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_siftagg_or_normal(q) $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO result_rows FROM r_on;
	SELECT count(*) INTO vs_normal_plan FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	RETURN NEXT;
END $$;

-- the ids as a CTE and again in an IN list: one restriction of dim_a_id
SELECT * FROM same($$
WITH s AS (SELECT id FROM scope)
SELECT e.dim_a_id, date_trunc('month', e.day)::date AS month, e.unit_id, avg(e.price)
FROM ev e JOIN s ON s.id = e.dim_a_id LEFT JOIN units u ON u.id = e.unit_id
WHERE e.latest AND u.flag IS NOT TRUE AND e.dim_a_id IN (SELECT id FROM s GROUP BY id) AND e.day >= '2024-03-01' AND e.day < '2025-03-01'
GROUP BY 1, 2, 3 $$);
-- the same restriction once
SELECT * FROM same($$
WITH s AS (SELECT id FROM scope)
SELECT e.dim_a_id, date_trunc('month', e.day)::date AS month, e.unit_id, avg(e.price)
FROM ev e JOIN s ON s.id = e.dim_a_id LEFT JOIN units u ON u.id = e.unit_id
WHERE e.latest AND u.flag IS NOT TRUE AND e.day >= '2024-03-01' AND e.day < '2025-03-01'
GROUP BY 1, 2, 3 $$);
-- all the entities: the scan reads them all anyway and keeps its plan
SELECT * FROM same($$
WITH s AS (SELECT id FROM scope_all)
SELECT e.dim_a_id, date_trunc('month', e.day)::date AS month, e.unit_id, avg(e.price)
FROM ev e JOIN s ON s.id = e.dim_a_id LEFT JOIN units u ON u.id = e.unit_id
WHERE e.latest AND u.flag IS NOT TRUE AND e.dim_a_id IN (SELECT id FROM s GROUP BY id) AND e.day >= '2024-03-01' AND e.day < '2025-03-01'
GROUP BY 1, 2, 3 $$);

DROP TABLE ev, units, scope, scope_all;
DROP FUNCTION same(text), plan_of(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
