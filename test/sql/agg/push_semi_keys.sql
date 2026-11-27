-- The semi join pushed into the scan of SiftAggV6 as an array of keys (EXISTS over one equality of two integer columns) is a join of one pair of columns: a second equality of the
-- subquery (another column of each side) stays with the join, whose rows the array does not decide. Every query is run with the node and without it (sift.enable_agg) and the two
-- answers are compared; the first word says they agree, the second whether the plan pushed the join into the scan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE fact (id int PRIMARY KEY, tenant_id int NOT NULL, country text NOT NULL, status text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO fact SELECT i, i % 50, (ARRAY['br', 'us', 'de', 'jp'])[1 + i % 4], (ARRAY['open', 'paid', 'void'])[1 + i % 3] FROM generate_series(1, 20000) i;
CREATE INDEX fact_col ON fact USING sift_col (id, tenant_id, country, status);
CREATE TABLE dim (id int NOT NULL, country text NOT NULL, kind int NOT NULL);
INSERT INTO dim SELECT i, (ARRAY['br', 'us'])[1 + i % 2], i % 3 FROM generate_series(0, 49) i;
INSERT INTO dim SELECT i, 'de', 1 FROM generate_series(0, 9) i;
VACUUM (FREEZE, ANALYZE) fact;
ANALYZE dim;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l; END LOOP;
	a := answer(q);
	SET LOCAL sift.enable_agg = off;
	b := answer(q);
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6Push%' THEN 'pushed' ELSE 'not pushed' END);
END $$;

SELECT q, same(q) FROM unnest(ARRAY[
	'SELECT status, count(*) FROM fact f WHERE EXISTS (SELECT 1 FROM dim d WHERE d.id = f.tenant_id) GROUP BY status',
	'SELECT status, count(*) FROM fact f WHERE EXISTS (SELECT 1 FROM dim d WHERE d.id = f.tenant_id AND d.country = f.country) GROUP BY status',
	'SELECT status, count(*) FROM fact f WHERE EXISTS (SELECT 1 FROM dim d WHERE d.id = f.tenant_id AND d.kind = 1) GROUP BY status',
	'SELECT status, count(*) FROM fact f WHERE f.tenant_id IN (SELECT d.id FROM dim d WHERE d.country = f.country) GROUP BY status',
	'SELECT status, count(*) FROM fact f WHERE NOT EXISTS (SELECT 1 FROM dim d WHERE d.id = f.tenant_id AND d.country = f.country) GROUP BY status'
]) q;

-- a fact so small that the normal plan is the cheaper one: sift.agg_force prices the pushed semi join as it does every scan of the aggregate
CREATE TABLE small_fact (id int PRIMARY KEY, tenant_id int NOT NULL, status text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO small_fact SELECT i, i % 50, (ARRAY['open', 'paid', 'void'])[1 + i % 3] FROM generate_series(1, 500) i;
CREATE INDEX small_fact_col ON small_fact USING sift_col (id, tenant_id, status);
VACUUM (FREEZE, ANALYZE) small_fact;
\set small_q 'SELECT status, count(*) FROM small_fact f WHERE f.tenant_id IN (SELECT d.id FROM dim d WHERE d.kind = 2) GROUP BY status'
SELECT same(:'small_q') AS normal_costs;
SET sift.agg_force = on;
SELECT same(:'small_q') AS forced;
RESET sift.agg_force;

DROP TABLE fact, dim, small_fact;
DROP FUNCTION same(text);
DROP FUNCTION answer(text);
DROP EXTENSION pg_sift;
