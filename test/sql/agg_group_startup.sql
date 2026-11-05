-- A sub-select that groups is planned for all its rows, so the first path of the groups that costs less in total used to drop the normal plan that gives its first group at
-- once. A LIMIT or a merge join above the sub-select wants that one: it reads a handful of groups, where the scan reads every unit before its first. The normal plan
-- stays a candidate next to the scan, and the plan with the lower cost for the rows the query asks for is the one. Each statement runs with the scan on and off (the rows
-- must be the same, in the same order where it has an ORDER BY), with no worker and with three.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE tenants (id int PRIMARY KEY, name text NOT NULL, set_id int);
INSERT INTO tenants SELECT g, 'tenant_' || lpad(g::text, 5, '0'), CASE WHEN g % 10 = 0 THEN NULL ELSE 1 + g % 5 END FROM generate_series(1, 4000) g;
-- link_id: NULL for a tenth of the rows and 1 to 400 for the rest (400 groups of equal size: ties); band_id moves with tenant_id, which the statistics do not know
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, category_id int NOT NULL, value_num numeric(10,2), link_id int, band_id int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + (g * 7919) % 4000, 1 + g % 19, CASE WHEN g % 13 = 0 THEN NULL ELSE (g % 500) / 5.0 END, CASE WHEN g % 10 = 0 THEN NULL ELSE 1 + g % 400 END,
       1 + ((g * 7919) % 4000) / 400 FROM generate_series(1, 60000) g;
CREATE TABLE notes (id serial PRIMARY KEY, tenant_id int NOT NULL, weight int);
INSERT INTO notes (tenant_id, weight) SELECT 1 + (g * 31) % 4000, g % 7 FROM generate_series(1, 8000) g;
CREATE INDEX events_tenant ON events (tenant_id, category_id);
CREATE INDEX events_link ON events (link_id);
CREATE INDEX notes_tenant ON notes (tenant_id);
CREATE INDEX events_col ON events USING sift_col (id, tenant_id, category_id, value_num, link_id, band_id) WITH (key_cut = tenant_id);
VACUUM (FREEZE, ANALYZE) events, tenants, notes;

CREATE TABLE startup_q (n int, label text, q text);
INSERT INTO startup_q VALUES
	(1, 'limit over a merge join on the key',  'SELECT t.name, a.n FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.tenant_id ORDER BY a.tenant_id LIMIT 10'),
	(2, 'limit, semi join of the keys',         'SELECT a.tenant_id, a.n FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a WHERE EXISTS (SELECT 1 FROM tenants t WHERE t.id = a.tenant_id AND t.set_id = 2) ORDER BY 1 LIMIT 10'),
	(3, 'limit, two groupings joined on the key', 'SELECT a.tenant_id, a.n, b.s FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a JOIN (SELECT tenant_id, sum(weight) AS s FROM notes GROUP BY 1) b ON b.tenant_id = a.tenant_id ORDER BY a.tenant_id LIMIT 20'),
	(4, 'limit over a window on the key',       'SELECT tenant_id, n, sum(n) OVER (ORDER BY tenant_id) AS running FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a ORDER BY 1 LIMIT 20'),
	(5, 'every group, hash join',               'SELECT t.name, a.n FROM (SELECT tenant_id, count(*) AS n, sum(value_num) AS s FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.tenant_id ORDER BY t.name'),
	(6, 'every group, regrouped by the dimension', 'SELECT t.set_id, sum(a.n) AS n FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.tenant_id GROUP BY 1 ORDER BY 1'),
	(7, 'groups ranked by the aggregate, limit', 'SELECT t.name, a.n FROM (SELECT tenant_id, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.tenant_id ORDER BY a.n DESC, t.name LIMIT 10'),
	(8, 'limit, nullable key, equal groups',    'SELECT t.name, a.n FROM (SELECT link_id, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.link_id ORDER BY a.link_id LIMIT 10'),
	(9, 'limit, ties in the order',             'SELECT t.name, a.n FROM (SELECT link_id, count(*) AS n FROM events GROUP BY 1) a JOIN tenants t ON t.id = a.link_id ORDER BY a.n, t.name LIMIT 10'),
	(10, 'union of two groupings, correlated filter', 'SELECT tenant_id, n FROM (SELECT tenant_id, count(*) AS n FROM events WHERE band_id < 4 GROUP BY 1 UNION ALL SELECT tenant_id, count(*) FROM events WHERE band_id >= 4 GROUP BY 1) u ORDER BY 1, 2 LIMIT 50');
-- the plan of each statement: the normal plan, or the number of scans (SiftAgg) in it; the rows of the two equal, with no worker and with three
CREATE FUNCTION startup_plan(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scans int := 0;
BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scans := scans + (l LIKE '%Custom Scan (SiftAgg)%')::int;
	END LOOP;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN CASE WHEN scans = 0 THEN 'normal' ELSE 'scan x' || scans END;
END $$;
CREATE FUNCTION startup_rows(q text, scan bool, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, true);
	EXECUTE format('SELECT string_agg(t::text, E''\n'') FROM (%s) t', q) INTO r;
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	RETURN r;
END $$;
SELECT n, label, startup_plan(q, true) AS plan, startup_rows(q, true, 0) = startup_rows(q, false, 0) AS same, startup_rows(q, true, 3) = startup_rows(q, false, 0) AS same_workers FROM startup_q ORDER BY n;

DROP FUNCTION startup_plan(text, bool);
DROP FUNCTION startup_rows(text, bool, int);
DROP TABLE startup_q, notes, events, tenants;
DROP EXTENSION pg_sift;
