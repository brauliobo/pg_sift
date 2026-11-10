-- A scope copy partitioned by its scope key (sift.scope_create(.., partition => true)) has one sift_col index per partition. A statement on one key reads one partition and is priced
-- for that partition, not for the whole copy: the largest tenant (the one most likely to be priced like the table) keeps the scan for every class, the small ones follow the cost
-- of their own size, and every answer equals the normal plan's. Plans and rows only.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE facts (id bigserial PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL)
	WITH (autovacuum_enabled = false);
-- 260 ids of 100 facts each: ids 1..150 are in tenant 1 (large), 151..180 in tenant 2, 181..183 in tenant 3 (small); tenants 4..9 hold one id each,
-- id 1 is also in tenant 2 and id 151 in tenant 3 (two keys), ids 241..260 are in no tenant (no copy of their facts)
INSERT INTO facts (dim_a_id, created_at, kind, amount)
SELECT 1 + i % 260, timestamptz '2026-01-01' + (i * 37 % 8760) * interval '1 hour', i % 7, (i * 13 % 1000)::numeric / 10 FROM generate_series(1, 26000) i;
CREATE TABLE pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id)) WITH (autovacuum_enabled = false);
INSERT INTO pairs SELECT m, 1 FROM generate_series(1, 150) m;
INSERT INTO pairs SELECT m, 2 FROM generate_series(151, 180) m;
INSERT INTO pairs SELECT m, 3 FROM generate_series(181, 183) m;
INSERT INTO pairs SELECT 190 + t, t FROM generate_series(4, 9) t;
INSERT INTO pairs VALUES (1, 2), (151, 3);
SET client_min_messages = warning;
SELECT sift.scope_create('facts_by_tenant', 'facts', 'dim_a_id', 'pairs', 'tenant_id', partition => true) > 0 AS built;
RESET client_min_messages;
SET synchronous_commit = on;
VACUUM (FREEZE, ANALYZE) facts_by_tenant;
SELECT relname, reltuples::int AS rows FROM pg_class WHERE relname ~ '^facts_by_tenant_p[1-3]$' ORDER BY relname;

CREATE FUNCTION plan_kind(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'scan'; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE TABLE classes (n int PRIMARY KEY, class text, q text);
INSERT INTO classes VALUES
	(1, 'sum and count by kind', 'SELECT kind, count(*), sum(amount) FROM %1$s WHERE tenant_id = %2$s GROUP BY kind'),
	(2, 'sum by kind, one month', 'SELECT kind, count(*), sum(amount) FROM %1$s WHERE tenant_id = %2$s AND created_at >= ''2026-06-01'' AND created_at < ''2026-07-01'' GROUP BY kind'),
	(3, 'distinct ids by month', 'SELECT date_trunc(''month'', created_at) AS m, count(DISTINCT dim_a_id) FROM %1$s WHERE tenant_id = %2$s GROUP BY 1'),
	(4, 'top 20 ids by sum', 'SELECT dim_a_id, sum(amount) AS s FROM %1$s WHERE tenant_id = %2$s GROUP BY 1 ORDER BY s DESC, dim_a_id LIMIT 20'),
	(5, 'latest 20', 'SELECT id, created_at, kind, amount FROM %1$s WHERE tenant_id = %2$s ORDER BY created_at DESC, id LIMIT 20');

-- the plan of each class for a large, a middle and a small tenant, and the answer against the normal plan
SELECT t.tenant, c.class, plan_kind(format(c.q, 'facts_by_tenant', t.tenant)) AS plan,
       sift_helpers.differences_agg(format(c.q, 'facts_by_tenant', t.tenant)) AS vs_normal_plan
FROM (VALUES (1), (2), (3)) t(tenant), classes c ORDER BY t.tenant, c.n;

-- a tenant with one id and a tenant that has no partition
SELECT sift_helpers.differences_agg(format(c.q, 'facts_by_tenant', 4)) AS one_id, sift_helpers.differences_agg(format(c.q, 'facts_by_tenant', 99)) AS no_partition
FROM classes c WHERE c.n = 1;

SELECT sift.scope_drop('facts_by_tenant');
DROP FUNCTION plan_kind(text);
DROP TABLE classes, facts, pairs;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
