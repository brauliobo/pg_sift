-- A scalar sub-select that looks one row up in a table by its whole unique key, and an EXISTS, in the select list, the GROUP BY or an aggregate (its argument or FILTER) of a grouped query
-- cost one probe for each row of the outer table. They become a left join of the table (of the keys of the table that EXISTS asks), which the scan reads in its pass: a row with no match
-- is NULL, as the sub-select is. The rows must be the same as with the scan off, for outer keys that are NULL, that repeat and that have no row in the table, with no worker and with three.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE tenants (id int PRIMARY KEY, region int, name text NOT NULL, UNIQUE (region, id));
INSERT INTO tenants SELECT g, CASE WHEN g % 7 = 0 THEN NULL ELSE 1 + g % 5 END, 'tenant_' || g FROM generate_series(1, 3000) g;
CREATE UNIQUE INDEX tenants_name ON tenants (name);
CREATE TABLE codes (a int, b int, label text, PRIMARY KEY (a, b));
INSERT INTO codes SELECT g % 20, g / 20, 'code_' || g FROM generate_series(0, 399) g;
CREATE TABLE marks (tenant_id int NOT NULL, weight int);
INSERT INTO marks SELECT 1 + (g * 17) % 2000, g % 5 FROM generate_series(1, 6000) g;
CREATE INDEX marks_tenant ON marks (tenant_id);
-- tenant_id: NULL for a tenth of the rows, and 3001 to 3100 are tenants that do not exist
CREATE TABLE events (id int PRIMARY KEY, tenant_id int, category_id int NOT NULL, a int NOT NULL, b int NOT NULL, amount numeric(10,2)) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, CASE WHEN g % 10 = 0 THEN NULL WHEN g % 13 = 0 THEN 3001 + g % 100 ELSE 1 + (g * 7919) % 3000 END, 1 + g % 6, g % 20, g % 19,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g % 500) / 5.0 END FROM generate_series(1, 40000) g;
CREATE INDEX events_col ON events USING sift_col (id, tenant_id, category_id, a, b, amount);
VACUUM (FREEZE, ANALYZE) events, tenants, codes, marks;

CREATE TABLE decor_q (n int, label text, q text);
INSERT INTO decor_q VALUES
	(1, 'sum of a looked up column',          'SELECT category_id, sum((SELECT t.region FROM tenants t WHERE t.id = e.tenant_id)) AS s FROM events e GROUP BY 1 ORDER BY 1'),
	(2, 'a looked up column as the group key', 'SELECT (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) AS r, count(*) AS n, sum(e.amount) AS s FROM events e GROUP BY 1 ORDER BY 1'),
	(3, 'count filtered by EXISTS',           'SELECT category_id, count(*) AS n, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM marks m WHERE m.tenant_id = e.tenant_id)) AS marked FROM events e GROUP BY 1 ORDER BY 1'),
	(4, 'a condition of its own on the table', 'SELECT category_id, count(*) AS n, count((SELECT t.name FROM tenants t WHERE t.id = e.tenant_id AND t.region > 2)) AS c FROM events e GROUP BY 1 ORDER BY 1'),
	(5, 'a key of two columns',                'SELECT category_id, count(*) AS n, max((SELECT c.label FROM codes c WHERE c.a = e.a AND c.b = e.b)) AS l FROM events e GROUP BY 1 ORDER BY 1'),
	(6, 'an expression of the looked up column', 'SELECT category_id, sum((SELECT coalesce(t.region, -1) FROM tenants t WHERE t.id = e.tenant_id)) AS s FROM events e GROUP BY 1 ORDER BY 1'),
	(7, 'first row of a column that is not unique', 'SELECT category_id, count((SELECT t.id FROM tenants t WHERE t.region = e.a + 1 ORDER BY t.id LIMIT 1)) AS c FROM events e GROUP BY 1 ORDER BY 1'),
	(8, 'in the select list, no grouping',    'SELECT e.id, (SELECT t.name FROM tenants t WHERE t.id = e.tenant_id) AS name FROM events e WHERE e.id <= 40 ORDER BY 1'),
	(9, 'compared in the WHERE',               'SELECT category_id, count(*) AS n FROM events e WHERE (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) = 2 GROUP BY 1 ORDER BY 1'),
	(10, 'a lateral left join',                'SELECT s.region, count(*) AS n FROM events e LEFT JOIN LATERAL (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) s ON true GROUP BY 1 ORDER BY 1'),
	(11, 'a lateral inner join',               'SELECT s.region, count(*) AS n FROM events e JOIN LATERAL (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) s ON true GROUP BY 1 ORDER BY 1'),
	(12, 'a filter and a lookup together',     'SELECT (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) AS r, count(*) FILTER (WHERE e.amount > 50) AS big, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM marks m WHERE m.tenant_id = e.tenant_id AND m.weight = 3)) AS marked FROM events e GROUP BY 1 ORDER BY 1'),
	(13, 'an expression of two looked up columns', 'SELECT category_id, count(*) AS n, max((SELECT coalesce(t.name, ''none'') || t.id::text FROM tenants t WHERE t.id = e.tenant_id)) AS m FROM events e GROUP BY 1 ORDER BY 1'),
	(14, 'an expression that is NULL for a NULL', 'SELECT category_id, sum((SELECT t.region * 2 + 1 FROM tenants t WHERE t.id = e.tenant_id)) AS s, count((SELECT t.region * 2 FROM tenants t WHERE t.id = e.tenant_id)) AS c FROM events e GROUP BY 1 ORDER BY 1');
CREATE FUNCTION decor_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scans int := 0; subs int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scans := scans + (l LIKE '%Custom Scan (SiftAgg)%')::int;
		subs := subs + (l LIKE '%SubPlan%')::int;
	END LOOP;
	RETURN CASE WHEN scans > 0 THEN 'scan x' || scans WHEN subs > 0 THEN 'probes' ELSE 'normal' END;
END $$;
CREATE FUNCTION decor_rows(q text, scan bool, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	EXECUTE format('SELECT coalesce(string_agg(t::text, E''\n'' ORDER BY t::text), '''') FROM (%s) t', q) INTO r;
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN r;
END $$;
SELECT n, label, decor_plan(q) AS plan, decor_rows(q, false, 0) <> '' AS has_rows, decor_rows(q, true, 0) = decor_rows(q, false, 0) AS same, decor_rows(q, true, 3) = decor_rows(q, false, 0) AS same_workers FROM decor_q ORDER BY n;

DROP FUNCTION decor_plan(text);
DROP FUNCTION decor_rows(text, bool, int);
DROP TABLE decor_q, events, marks, codes, tenants;
DROP EXTENSION pg_sift;
