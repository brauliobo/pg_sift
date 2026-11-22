-- The planner chooses between the hybrid node and PostgreSQL's own plan by cost. A tenant whose ACL names a few items, which a btree probe reads in a fraction of a millisecond, keeps the standard plan; a large one, a generic plan and a plan without nested loops take the hybrid node. Every result is compared with the standard plan.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
CREATE TABLE ct_docs (id int PRIMARY KEY, region text NOT NULL, amount numeric(10,2) NOT NULL, body text NOT NULL);
INSERT INTO ct_docs
SELECT i, 'r' || (i % 6), ((i * 37) % 100000) / 100.0,
	concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, CASE WHEN i % 4000 = 7 THEN 'rare' END, 'w' || (i % 29))
FROM generate_series(1, 40000) i;
CREATE TABLE ct_acl (tenant_id int NOT NULL, item_id int NOT NULL);
INSERT INTO ct_acl SELECT 1, i FROM generate_series(1, 40) i;
INSERT INTO ct_acl SELECT 2, i FROM generate_series(1, 40000, 3) i;
INSERT INTO ct_acl SELECT 3, i FROM generate_series(1, 40000) i WHERE i % 20 IN (6, 7);
CREATE INDEX ct_acl_tenant ON ct_acl (tenant_id);
CREATE INDEX ct_text ON ct_docs USING sift (body);
CREATE INDEX ct_col ON ct_docs USING sift_col (id, region, amount);
ANALYZE ct_docs;
ANALYZE ct_acl;
VACUUM (FREEZE) ct_docs;
RESET client_min_messages;

CREATE FUNCTION ct_node(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; n text := 'standard';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftHybrid' THEN n := substring(l from 'Custom Scan \((SiftHybrid[A-Za-z]+)\)'); END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION ct_cmp(a text, b text, OUT rows bigint, OUT diff bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE 'CREATE TEMP TABLE ct_a AS ' || a;
	EXECUTE 'CREATE TEMP TABLE ct_b AS ' || b;
	SELECT count(*) INTO rows FROM ct_a;
	SELECT count(*) INTO diff FROM ((TABLE ct_a EXCEPT ALL TABLE ct_b) UNION ALL (TABLE ct_b EXCEPT ALL TABLE ct_a)) d;
	DROP TABLE ct_a, ct_b;
END $$;

CREATE TABLE ct_q (name text PRIMARY KEY, tenant int, term text);
INSERT INTO ct_q VALUES ('small_common', 1, 'common'), ('small_rare', 1, 'rare'), ('large_common', 2, 'common'), ('large_rare', 2, 'rare'), ('mid_common', 3, 'common'), ('mid_rare', 3, 'rare');

CREATE FUNCTION ct_sql(tenant int, term text, std boolean) RETURNS text LANGUAGE sql AS
$$ SELECT format('SELECT region, count(*), sum(amount), avg(amount), max(amount) FROM ct_docs c WHERE %s EXISTS (SELECT 1 FROM ct_acl a WHERE a.tenant_id = %s AND a.item_id = c.id) AND body @@> %L GROUP BY region',
	CASE WHEN std THEN 'random() < 2 AND' ELSE '' END, tenant, term) $$;

SELECT name, ct_node(ct_sql(tenant, term, false)) AS custom_plan, (ct_cmp(ct_sql(tenant, term, false), ct_sql(tenant, term, true))).* FROM ct_q ORDER BY name;

SET enable_nestloop = off;
SELECT name, ct_node(ct_sql(tenant, term, false)) AS without_nested_loop FROM ct_q ORDER BY name;
RESET enable_nestloop;

SET enable_indexscan = off;
SELECT name, ct_node(ct_sql(tenant, term, false)) AS without_index_scan FROM ct_q ORDER BY name;
RESET enable_indexscan;

-- a cold heap makes the probe real reads: the hybrid node takes the small tenants too
SET effective_cache_size = '8kB';
SELECT name, ct_node(ct_sql(tenant, term, false)) AS cold_heap FROM ct_q ORDER BY name;
RESET effective_cache_size;

PREPARE ct_p(int, text) AS SELECT region, count(*), sum(amount), avg(amount), max(amount) FROM ct_docs c WHERE EXISTS (SELECT 1 FROM ct_acl a WHERE a.tenant_id = $1 AND a.item_id = c.id) AND body @@> $2 GROUP BY region;
PREPARE ct_p_std(int, text) AS SELECT region, count(*), sum(amount), avg(amount), max(amount) FROM ct_docs c WHERE random() < 2 AND EXISTS (SELECT 1 FROM ct_acl a WHERE a.tenant_id = $1 AND a.item_id = c.id) AND body @@> $2 GROUP BY region;
SET plan_cache_mode = force_generic_plan;
SELECT ct_node('EXECUTE ct_p(1, ''common'')') AS generic_small, ct_node('EXECUTE ct_p(2, ''rare'')') AS generic_large;
SELECT q.name, (ct_cmp(format('EXECUTE ct_p(%s, %L)', q.tenant, q.term), format('EXECUTE ct_p_std(%s, %L)', q.tenant, q.term))).* FROM ct_q q ORDER BY q.name;
RESET plan_cache_mode;
DROP FUNCTION ct_cmp(text, text), ct_node(text), ct_sql(integer, text, boolean);
DROP TABLE ct_docs, ct_q, ct_acl;
DROP EXTENSION pg_sift;
