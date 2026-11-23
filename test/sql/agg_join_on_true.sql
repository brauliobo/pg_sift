-- A join with no condition (ON true, CROSS JOIN, a comma) to a VALUES list, a sub-select, a function or a table is a lookup of every row of the right side
-- for each tuple of the scan: the rows are those of the normal plan (a duplicate or NULL row of the right side counts as often as there is one), and the
-- right side stays under half of work_mem. The table on the nullable side of a RIGHT or FULL join keeps the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE tenants (id int PRIMARY KEY, kind text);
INSERT INTO tenants SELECT i, 'k' || i % 4 FROM generate_series(1, 40) i;
CREATE TABLE orders (id int PRIMARY KEY, tenant_id int, status text, qty int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO orders SELECT i, 1 + i % 40, (ARRAY['open', 'paid', 'void'])[1 + i % 3], i % 17, CASE WHEN i % 29 = 0 THEN now() END FROM generate_series(1, 6000) i;
CREATE INDEX orders_col ON orders USING sift_col (id, tenant_id, status, qty, deleted_at);
VACUUM (FREEZE, ANALYZE) orders;
ANALYZE tenants;

-- the rows of the query with the extension on and off, how many differ, and the scans of the index in the plan
CREATE FUNCTION check_query(q text) RETURNS TABLE (n bigint, diff bigint, scans int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS ' || q;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS ' || q;
	SELECT count(*) INTO diff FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	SELECT count(*) INTO n FROM r_off;
	SET LOCAL sift.enable_agg = on;
	scans := 0;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg' THEN scans := scans + 1; END IF;
	END LOOP;
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- a VALUES list, a duplicate row of it, a NULL row
SELECT * FROM check_query($q$SELECT m.k, count(*), sum(o.qty) FROM orders o CROSS JOIN (VALUES (1), (2), (2)) m(k) GROUP BY m.k$q$);
SELECT * FROM check_query($q$SELECT m.k, count(*), sum(o.qty) FROM orders o JOIN (VALUES (1), (NULL)) m(k) ON true GROUP BY m.k$q$);
SELECT * FROM check_query($q$SELECT m.k, count(*), sum(o.qty) FROM orders o LEFT JOIN (VALUES (1), (NULL)) m(k) ON true GROUP BY m.k$q$);
-- a sub-select with LIMIT, with GROUP BY, a function
SELECT * FROM check_query($q$SELECT s.kind, count(*), sum(o.qty) FROM orders o JOIN (SELECT kind FROM tenants ORDER BY id LIMIT 1) s ON true GROUP BY s.kind$q$);
SELECT * FROM check_query($q$SELECT s.kind, s.n, count(*) FROM orders o JOIN (SELECT kind, count(*) AS n FROM tenants GROUP BY kind) s ON true GROUP BY s.kind, s.n$q$);
SELECT * FROM check_query($q$SELECT s.g, o.status, count(*) FROM orders o, (SELECT g FROM generate_series(1, 3) g) s WHERE o.qty < 5 GROUP BY s.g, o.status$q$);
-- a table, with a condition on it, and with the equality join of another table as well
SELECT * FROM check_query($q$SELECT t.kind, count(*), sum(o.qty) FROM orders o CROSS JOIN tenants t WHERE t.id <= 3 AND o.deleted_at IS NULL GROUP BY t.kind$q$);
SELECT * FROM check_query($q$SELECT s.g, t.kind, count(*) FROM orders o JOIN (SELECT g FROM generate_series(1, 3) g) s ON true JOIN tenants t ON t.id = o.tenant_id GROUP BY s.g, t.kind$q$);
-- a right side with no row, ungrouped
SELECT * FROM check_query($q$SELECT count(*), sum(o.qty) FROM orders o JOIN (SELECT 1 AS k WHERE false) m ON true$q$);
SELECT * FROM check_query($q$SELECT count(o.id), m.k FROM orders o LEFT JOIN (SELECT 1 AS k WHERE false) m ON true GROUP BY m.k$q$);
SELECT * FROM check_query($q$SELECT count(*), sum(o.qty) FROM orders o, (SELECT g FROM generate_series(1, 5) g) s$q$);
-- the nullable side of an outer join keeps the normal plan
SELECT * FROM check_query($q$SELECT m.k, count(*) FROM (VALUES (1), (2)) m(k) LEFT JOIN orders o ON true GROUP BY m.k$q$);

DROP FUNCTION check_query(text);
DROP TABLE orders, tenants;
DROP EXTENSION pg_sift;
