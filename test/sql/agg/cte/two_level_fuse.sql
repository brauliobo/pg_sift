-- A CTE that several aggregates read, at several granularities of one series, is planned inline when the scans cost less than the work they replace
-- (agg/cte/hook.c), the arms read the index in one pass, and the rows are the normal plan's. The per-item form with two distinct counts (the rows, and another
-- column of them that the arms group by) is one scan too.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE orders (id int PRIMARY KEY, tenant_id int, placed_at date, deleted_at timestamptz, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO orders
SELECT i, 1 + i % 30, date '2025-01-01' + (i * 7919) % 400, CASE WHEN i % 31 = 0 THEN now() END,
       (SELECT jsonb_agg(jsonb_build_object('category', 'cat' || (i + j) % 4, 'qty', (i + j) % 7)) FROM generate_series(1, 1 + i % 4) j)
FROM generate_series(1, 4000) i;
CREATE INDEX orders_col ON orders USING sift_col (id, tenant_id, placed_at, deleted_at, items) WITH (paths = 'items[]{category text, qty int4}');
VACUUM (FREEZE, ANALYZE) orders;

-- the rows of the query with the extension on and off, how many differ, the scans of the index in the plan, whether a CTE is still read as a table, and the arms of one pass
CREATE FUNCTION check_query(q text) RETURNS TABLE (n bigint, diff bigint, scans int, cte_scan bool, one_pass int) LANGUAGE plpgsql AS $$
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
	cte_scan := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg' THEN scans := scans + 1; END IF;
		IF l ~ 'CTE Scan' THEN cte_scan := true; END IF;
	END LOOP;
	one_pass := sift_helpers.agg_arms_in_one_pass(q);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- a series at four granularities: every arm a GROUP BY of the one CTE
SELECT * FROM check_query($q$WITH base AS (
  SELECT o.id AS order_id, o.placed_at, i.qty FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int)
  WHERE o.tenant_id IN (1, 2, 3, 4, 5, 6, 7, 8) AND o.deleted_at IS NULL)
SELECT 'day' AS grain, placed_at AS bucket, count(DISTINCT order_id) AS orders, sum(qty) AS units FROM base GROUP BY 2
UNION ALL SELECT 'week', date_trunc('week', placed_at)::date, count(DISTINCT order_id), sum(qty) FROM base GROUP BY 2
UNION ALL SELECT 'month', date_trunc('month', placed_at)::date, count(DISTINCT order_id), sum(qty) FROM base GROUP BY 2
UNION ALL SELECT 'quarter', date_trunc('quarter', placed_at)::date, count(DISTINCT order_id), sum(qty) FROM base GROUP BY 2$q$);
-- two granularities with another measure in each arm
SELECT * FROM check_query($q$WITH base AS (
  SELECT o.id AS order_id, o.placed_at, i.category, i.qty FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int)
  WHERE o.deleted_at IS NULL)
SELECT 'month' AS grain, date_trunc('month', placed_at)::date AS bucket, count(*) AS n, max(qty) AS m FROM base GROUP BY 2
UNION ALL SELECT 'category', NULL, count(*), max(qty) FROM base GROUP BY category$q$);

-- per-item arms with a second count(DISTINCT) over another column of the row: the rows and the tenant, in one scan
SELECT * FROM check_query($q$WITH b AS (
  SELECT o.id AS rid, o.tenant_id AS pid, i.category AS k, i.qty AS n FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int)
  WHERE o.deleted_at IS NULL)
SELECT k, sum(a) AS s, count(DISTINCT rid) AS rows, count(DISTINCT pid) AS tenants
FROM (SELECT rid, pid, k, sum(n) AS a FROM b GROUP BY rid, pid, k UNION ALL SELECT rid, pid, NULL::text, sum(n) FROM b GROUP BY rid, pid) x GROUP BY k$q$);
SELECT * FROM check_query($q$WITH b AS (
  SELECT o.id AS rid, o.tenant_id AS pid, i.category AS k, i.qty AS n FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int)
  WHERE o.deleted_at IS NULL)
SELECT count(DISTINCT rid) AS rows, count(DISTINCT pid) AS tenants, sum(a) AS s
FROM (SELECT rid, pid, k, sum(n) AS a FROM b GROUP BY rid, pid, k UNION ALL SELECT rid, pid, NULL::text, sum(n) FROM b GROUP BY rid, pid) x$q$);

DROP FUNCTION check_query(text);
DROP TABLE orders;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
