-- A ranked search restricted by a subquery on another table (the items of a tenant). The planner prices the
-- ranked SiftScan from the matches the index counts, also on a key whose index has fields (where `@@>` is the call
-- sift.match_idx, which has no RESTRICT of its own), and prices draining it past its limit hint at what that costs.
-- A join that keeps enough of the matches takes them from the scan with a Nested Loop Semi Join, which stops at the
-- limit; one that keeps few drains the scan, and a hash or merge join is cheaper. The answers (rows, scores, ties)
-- are those of the scan without a join under every plan.
CREATE EXTENSION pg_sift;

\ir test/sift_plan.sql

-- rows the planner expects of a query against the rows it returns
CREATE FUNCTION est_ratio(q text) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE
	plan json;
	act numeric;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO plan;
	EXECUTE 'SELECT count(*) FROM (' || q || ') t' INTO act;
	RETURN round((plan->0->'Plan'->>'Plan Rows')::numeric / act, 1);
END $$;

-- a cost of the SiftScan node of a query
CREATE FUNCTION scan_cost(q text, what text) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE
	plan json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO plan;
	RETURN (jsonb_path_query_first(plan::jsonb, '$.** ? (@."Node Type" == "Custom Scan")') ->> what)::numeric;
END $$;

-- 30000 items: 'honey' in 2 of 5 names, 'helion' in 1 of 500; tenant 1 owns 2% of them, tenant 2 half, tenant 3 twenty.
-- items_plain holds the same names under a plain index, items the sections of a name and a city under an index with fields
CREATE TABLE items (id int PRIMARY KEY, name text, city text, pad text) WITH (autovacuum_enabled = off);
INSERT INTO items SELECT i, CASE WHEN i % 5 < 2 THEN repeat('honey ', 1 + i % 3) ELSE '' END || CASE WHEN i % 500 = 100 THEN 'helion ' ELSE '' END
	|| 'w' || i % 997 || ' x' || i % 31, 'city' || i % 50, repeat('p', 60) FROM generate_series(1, 30000) i;
CREATE TABLE items_plain (LIKE items INCLUDING ALL) WITH (autovacuum_enabled = off);
INSERT INTO items_plain SELECT * FROM items;
CREATE INDEX items_sift ON items USING sift ((name || chr(29) || city)) WITH (fields = 'name', prefix_index = 3);
CREATE INDEX items_plain_sift ON items_plain USING sift (name);
CREATE TABLE accounts (id serial PRIMARY KEY, tenant int, item_id int) WITH (autovacuum_enabled = off);
INSERT INTO accounts (tenant, item_id) SELECT v.t, i FROM generate_series(1, 30000) i,
	LATERAL (VALUES (1, i % 50 = 0), (2, i % 2 = 0), (3, i % 3000 IN (1, 100))) v(t, owns) WHERE v.owns;
CREATE INDEX accounts_tenant ON accounts (tenant);
CREATE INDEX accounts_item ON accounts (item_id);
CREATE TABLE catalogs (item_id int) WITH (autovacuum_enabled = off);
INSERT INTO catalogs SELECT i FROM generate_series(1, 30000) i WHERE i % 3 = 0;
CREATE INDEX catalogs_item ON catalogs (item_id);
ANALYZE items;
ANALYZE items_plain;
ANALYZE accounts;
ANALYZE catalogs;

-- the estimate of the matches is the index's for the plain index and for the call on the index with fields
SELECT est_ratio($$SELECT id FROM items_plain WHERE name @@> 'honey'$$) AS plain_honey,
	est_ratio($$SELECT id FROM items_plain WHERE name @@> 'helion'$$) AS plain_helion,
	est_ratio($$SELECT id FROM items WHERE (name || chr(29) || city) @@> 'honey'$$) AS fields_honey,
	est_ratio($$SELECT id FROM items WHERE (name || chr(29) || city) @@> 'helion'$$) AS fields_helion,
	est_ratio($$SELECT id FROM items WHERE (name || chr(29) || city) @@> 'name:honey'$$) AS fields_name;

-- a scan with a limit hint costs its k rows up to its startup, and drained costs what the scan without a limit does
SELECT round(scan_cost(q || ' LIMIT 20', 'Total Cost') / scan_cost(q, 'Total Cost'), 1) AS drained,
	scan_cost(q || ' LIMIT 20', 'Startup Cost') < scan_cost(q || ' LIMIT 20', 'Total Cost') / 10 AS startup_for_k
	FROM (VALUES ($$SELECT id FROM items WHERE (name || chr(29) || city) @@> 'honey' ORDER BY sift.score(ctid) DESC$$)) v(q);

-- the restriction to a tenant written as a subquery, as a correlated one and as a join with the tenant's distinct rows,
-- which are read through a left join; the answer is ordered by score and id, so that the ties are those of the id order
CREATE FUNCTION tenant_query(form text, tenant int, term text) RETURNS text LANGUAGE sql AS
$$ SELECT format(CASE form
	WHEN 'in' THEN $q$SELECT i.id, sift.score(i.ctid) FROM items i WHERE (i.name || chr(29) || i.city) @@> %2$L AND i.id IN
		(SELECT a.item_id FROM accounts a LEFT JOIN catalogs g ON g.item_id = a.item_id WHERE a.tenant = %1$s)
		ORDER BY sift.score(i.ctid) DESC, i.id LIMIT 20$q$
	WHEN 'exists' THEN $q$SELECT i.id, sift.score(i.ctid) FROM items i WHERE (i.name || chr(29) || i.city) @@> %2$L AND EXISTS
		(SELECT 1 FROM accounts a WHERE a.item_id = i.id AND a.tenant = %1$s) ORDER BY sift.score(i.ctid) DESC, i.id LIMIT 20$q$
	WHEN 'join' THEN $q$SELECT i.id, sift.score(i.ctid) FROM items i JOIN (SELECT DISTINCT a.item_id FROM accounts a
		LEFT JOIN catalogs g ON g.item_id = a.item_id WHERE a.tenant = %1$s) t ON t.item_id = i.id
		WHERE (i.name || chr(29) || i.city) @@> %2$L ORDER BY sift.score(i.ctid) DESC, i.id LIMIT 20$q$
	END, tenant, term) $$;

CREATE FUNCTION tenant_shape(form text, tenant int, term text) RETURNS text LANGUAGE sql AS
$$ SELECT CASE WHEN p !~ 'SiftScan' THEN 'no scan' WHEN p ~ 'Nested Loop Semi Join' THEN 'ranked scan, nested loop semi join'
	ELSE 'ranked scan, other join' END FROM (SELECT sift_plan(tenant_query(form, tenant, term)) p) s $$;

-- the tenant owns half of the heap, so a few rows of the scan reach the limit: a nested loop, for a broad and a narrow term;
-- it owns twenty rows, fewer than the limit, so the scan is drained whatever the join: not a nested loop
SELECT f.form, w.tenant, w.term, tenant_shape(f.form, w.tenant, w.term)
	FROM (VALUES ('in'), ('exists')) f(form), (VALUES (2, 'honey'), (2, 'helion'), (3, 'honey')) w(tenant, term) ORDER BY 1, 2, 3;

-- every plan answers what the scan without a join answers, the ones the planner picks and the ones it does not
CREATE FUNCTION tenant_rows(form text, tenant int, term text, mode text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM set_config('enable_nestloop', CASE mode WHEN 'hash or merge' THEN 'off' ELSE 'on' END, true),
		set_config('enable_hashjoin', CASE mode WHEN 'nested loop' THEN 'off' ELSE 'on' END, true),
		set_config('enable_mergejoin', CASE mode WHEN 'nested loop' THEN 'off' ELSE 'on' END, true);
	EXECUTE format('SELECT string_agg(t::text, '','') FROM (%s) t', tenant_query(form, tenant, term)) INTO r;
	PERFORM set_config('enable_nestloop', 'on', true), set_config('enable_hashjoin', 'on', true), set_config('enable_mergejoin', 'on', true);
	RETURN r;
END $$;

CREATE FUNCTION tenant_base(tenant int, term text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	EXECUTE format($q$SELECT string_agg(t::text, ',') FROM (SELECT id, s FROM (SELECT i.id, sift.score(i.ctid) AS s FROM items i
		WHERE (i.name || chr(29) || i.city) @@> %L ORDER BY sift.score(i.ctid) DESC OFFSET 0) r
		WHERE id IN (SELECT item_id FROM accounts WHERE accounts.tenant = %s) ORDER BY s DESC, id LIMIT 20) t$q$, term, tenant) INTO r;
	RETURN r;
END $$;

SELECT f.form, m.mode, count(*) AS cells, count(*) FILTER (WHERE x.got IS DISTINCT FROM x.base) AS differ, count(*) FILTER (WHERE x.base IS NULL) AS empty
	FROM (VALUES ('in'), ('exists'), ('join')) f(form), (VALUES ('planner'), ('hash or merge'), ('nested loop')) m(mode),
	(VALUES (1), (2), (3)) t(tenant), (VALUES ('honey'), ('helion'), ('honey helion'), ('name:honey')) w(term),
	LATERAL (SELECT tenant_rows(f.form, t.tenant, w.term, m.mode) AS got, tenant_base(t.tenant, w.term) AS base) x
	GROUP BY 1, 2 ORDER BY 1, 2;

DROP FUNCTION tenant_base(int, text), tenant_rows(text, int, text, text), tenant_shape(text, int, text), tenant_query(text, int, text),
	scan_cost(text, text), est_ratio(text), sift_plan(text);
DROP TABLE items, items_plain, accounts, catalogs;
DROP EXTENSION pg_sift;
