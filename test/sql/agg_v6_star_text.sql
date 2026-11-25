-- A text query (body @@> 'q') next to a lookup join: the star scan of SiftAggV6 takes the hits of the text index as the rows it reads (a Search line next to its Lookup Joins), except for a semi join.
-- Each statement returns what the same statement does with a volatile qual (the plan of PostgreSQL), and the plan column says whether the scan has a Search and Lookup Joins together.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
CREATE TABLE st_docs (id int PRIMARY KEY, tenant int NOT NULL, region int NOT NULL, body text NOT NULL);
INSERT INTO st_docs SELECT i, i % 3000, i % 7, concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, CASE WHEN i % 20 = 0 THEN 'mid' END, 'base') FROM generate_series(1, 120000) i;
CREATE INDEX st_text ON st_docs USING sift (body);
CREATE INDEX st_col ON st_docs USING sift_col (id, tenant, region);
CREATE TABLE st_acl (tenant_id int NOT NULL, item_id int NOT NULL);
INSERT INTO st_acl SELECT 1, i FROM generate_series(1, 30) i;
INSERT INTO st_acl SELECT 2, i FROM generate_series(1, 120000, 3) i;
CREATE INDEX st_acl_tenant ON st_acl (tenant_id);
SET default_statistics_target = 500;
ANALYZE st_docs;
ANALYZE st_acl;
RESET default_statistics_target;
VACUUM st_docs;
RESET client_min_messages;
SET enable_nestloop = off;
SET effective_cache_size = '8kB';

CREATE FUNCTION st_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; search boolean := false; joins boolean := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		search := search OR l ~ '^\s*Search: ';
		joins := joins OR l ~ '^\s*Lookup Joins: ';
	END LOOP;
	RETURN CASE WHEN search AND joins THEN 'search with lookup joins' ELSE 'ok' END;
END $$;

CREATE FUNCTION st_diff(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE 'CREATE TEMP TABLE st_a AS ' || q;
	EXECUTE 'CREATE TEMP TABLE st_b AS ' || regexp_replace(q, 'WHERE ', 'WHERE random() < 2 AND ');
	SELECT count(*) INTO n FROM ((TABLE st_a EXCEPT ALL TABLE st_b) UNION ALL (TABLE st_b EXCEPT ALL TABLE st_a)) d;
	DROP TABLE st_a, st_b;
	RETURN n;
END $$;

CREATE TABLE st_q (name text PRIMARY KEY, q text);
INSERT INTO st_q VALUES
	('anti',	'SELECT region, count(*) FROM st_docs c WHERE c.body @@> ''mid'' AND c.tenant < 200 AND NOT EXISTS (SELECT 1 FROM st_acl a WHERE a.tenant_id = 1 AND a.item_id = c.id) GROUP BY region'),
	('anti_all',	'SELECT region, count(*) FROM st_docs c WHERE c.body @@> ''mid'' AND NOT EXISTS (SELECT 1 FROM st_acl a WHERE a.tenant_id = 1 AND a.item_id = c.id) GROUP BY region'),
	('semi',	'SELECT region, count(*) FROM st_docs c WHERE c.body @@> ''mid'' AND c.tenant < 200 AND EXISTS (SELECT 1 FROM st_acl a WHERE a.tenant_id = 2 AND a.item_id = c.id) GROUP BY region'),
	('inner',	'SELECT region, count(*) FROM st_docs c JOIN st_acl a ON a.item_id = c.id WHERE c.body @@> ''mid'' AND a.tenant_id = 2 GROUP BY region');
SELECT name, st_plan(q) AS plan, st_diff(q) AS diff FROM st_q ORDER BY name;
SELECT region, count(*) FROM st_docs c WHERE c.body @@> 'mid' AND NOT EXISTS (SELECT 1 FROM st_acl a WHERE a.tenant_id = 1 AND a.item_id = c.id) GROUP BY region ORDER BY region;
DROP FUNCTION st_plan(text), st_diff(text);
DROP TABLE st_docs, st_acl, st_q;
DROP EXTENSION pg_sift;
