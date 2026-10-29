-- A rewrite (CLUSTER, VACUUM FULL, pg_repack) leaves the visibility map of the table empty: the scan checks the heap for every row it keeps, the planner
-- prices that (the pages of the kept units that hold a selected row, each at the price of a page read), and a normal plan that is close wins until a
-- VACUUM. sift.explain_why and sift.explain_refusals say so.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

-- rows in key order, seven to a page: a key range is a range of heap pages, and the btree answers the same range
CREATE TABLE ordered (id int, k int, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ordered SELECT i, i / 300, i % 7, repeat('x', 1000) FROM generate_series(0, 59999) i;
CREATE INDEX ordered_k ON ordered (k);
CREATE INDEX ordered_col ON ordered USING sift_col (id, k, v);
CLUSTER ordered USING ordered_k;
-- the statistics of every row, not of a random sample: the cost of the normal plan moved by 15% from one ANALYZE to the next, and the refusals below
-- are decided by a few percent. k < 6 sits 16% above the btree's price with the empty map and 20% below it after the VACUUM (k < 7 was 1% above).
SET default_statistics_target = 1000;
ANALYZE ordered;
RESET default_statistics_target;

CREATE FUNCTION why(q text) RETURNS TABLE (used bool, slug text, reason text) LANGUAGE sql AS $$
	SELECT used, slug, regexp_replace(reason, '[0-9]+(\.[0-9]+)?', 'N', 'g') FROM sift.explain_why(q)
$$;

CREATE FUNCTION cte_why(q text) RETURNS TABLE (used bool, slug text, reason text) LANGUAGE sql AS $$
	SELECT used, slug, regexp_replace(reason, '[0-9]+(\.[0-9]+)?', 'N', 'g') FROM sift.explain_why(q) WHERE arm = 'cte_inline'
$$;

-- what the empty map adds to the scan is bounded by the pages of the units the zones keep: a tenth of the table (k < 20 keeps 10% of the units)
-- charges less than half of what the whole table (k < 300) does
CREATE TABLE costs AS
SELECT k_below, (SELECT scan_cost FROM sift.explain_why(format('SELECT v, count(*), sum(k) FROM ordered WHERE k < %s GROUP BY v', k_below))) AS empty_cost
FROM unnest(ARRAY[20, 300]) k_below;

-- a configuration that prices a sequential page above a random one (seq_page_cost 100, random_page_cost 4) prices the check of those pages as the random fetch
-- of them, not at the sequential price: the scan of a table too large to be cached costs the same at 100 as at 4, and more than at 1
CREATE TABLE page_costs (seq float8, cost float8);
SET sift.plan_scale = 1000;
DO $$ DECLARE s float8; BEGIN
	FOREACH s IN ARRAY ARRAY[1, 4, 100]::float8[] LOOP
		PERFORM set_config('seq_page_cost', s::text, false);
		INSERT INTO page_costs SELECT s, scan_cost FROM sift.explain_why('SELECT v, count(*), sum(k) FROM ordered WHERE k < 300 GROUP BY v');
	END LOOP;
END $$;
RESET seq_page_cost;
RESET sift.plan_scale;
SELECT (SELECT cost FROM page_costs WHERE seq = 100) = (SELECT cost FROM page_costs WHERE seq = 4) AS capped_at_random,
	(SELECT cost FROM page_costs WHERE seq = 1) < (SELECT cost FROM page_costs WHERE seq = 4) AS cheaper_when_sequential_is;
DROP TABLE page_costs;

-- empty map: refused for its cost, with the pages that are not all-visible and the way out
SELECT * FROM why('SELECT v, count(*), sum(k) FROM ordered WHERE k < 6 GROUP BY v');
-- the share of the pages is the table's, not the query's
SELECT used, slug, reason ~ '^the normal plan is estimated cheaper than the scan \(the scan costs [0-9.]+, the normal plan [0-9.]+; 100% of the heap pages' AS pages_100
FROM sift.explain_why('SELECT v, count(*), sum(k) FROM ordered WHERE k < 6 GROUP BY v');
-- a scan that loses for another reason says nothing of the map: one key of a table the btree reads at once
SELECT used, slug, reason !~ 'all-visible' AS no_hint FROM sift.explain_why('SELECT count(*) FROM ordered WHERE k = 5');

-- the same for a shared CTE that two aggregates read: the inlined scans pay the heap check and lose to the shared plan
SELECT * FROM cte_why('WITH r AS (SELECT * FROM ordered WHERE k < 12) SELECT a.v, a.n, b.s FROM (SELECT v, count(*) AS n FROM r GROUP BY v) a JOIN (SELECT v, sum(k) AS s FROM r GROUP BY v) b ON a.v = b.v');
-- the scans of the inline plan that lost are not in the plan of the statement, and its cte_inline row prices the alternative that is: the plan the planner
-- makes without the library, not the shared CTE plan with the rows its aggregates lost charged
CREATE FUNCTION plain_total(q text) RETURNS float8 LANGUAGE plpgsql SET sift.enable_agg = off AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN (j->0->'Plan'->>'Total Cost')::float8;
END $$;
\set q 'WITH r AS (SELECT * FROM ordered WHERE k < 12) SELECT a.v, a.n, b.s FROM (SELECT v, count(*) AS n FROM r GROUP BY v) a JOIN (SELECT v, sum(k) AS s FROM r GROUP BY v) b ON a.v = b.v'
-- a statement planned from its cached verdict has no arms of the variants
SET sift.agg_verdict_cache = off;
SELECT arm, used, slug, CASE WHEN arm = 'cte_inline' THEN round(normal_cost::numeric, 2) = round(plain_total(:'q')::numeric, 2) END AS against_plain
FROM sift.explain_why(:'q') WHERE arm ~ 'inline' ORDER BY arm, scan_cost;
RESET sift.agg_verdict_cache;
DROP FUNCTION plain_total(text);
-- no way out is promised when the shared plan pays the heap check too: a VACUUM would cut both
SELECT * FROM cte_why('WITH c AS (SELECT v, count(*) AS n, sum(k) AS s FROM ordered WHERE k < 28 GROUP BY v) SELECT a.v, a.n, b.s FROM c a JOIN c b ON a.v = b.v');

-- a plan that keeps the scans with an empty map reads the heap check it recorded for them after the plans that lost were deleted
SELECT used, slug FROM cte_why('WITH r AS (SELECT * FROM ordered WHERE k < 120 AND k >= 0) SELECT a.v, a.n, b.s FROM (SELECT v, count(*) AS n FROM r GROUP BY v) a JOIN (SELECT v, sum(k) AS s FROM r GROUP BY v) b ON a.v = b.v');
SELECT used, slug FROM why('SELECT v, count(*), sum(k) FROM ordered WHERE k < 200 GROUP BY v');

-- the heap check a plan recorded is not kept across statements: a refused CTE plan deletes its memory when the planner hook is done, and the next statement
-- of the transaction (a function plans several) adds its own to the list (a crash on a server with assertions, where freed memory is overwritten)
SET sift.explain_refusals = on;
SET client_min_messages = warning;
DO $$ DECLARE r record; BEGIN
	FOR r IN EXPLAIN WITH r AS (SELECT * FROM ordered WHERE k < 12) SELECT a.v, a.n, b.s FROM (SELECT v, count(*) AS n FROM r GROUP BY v) a JOIN (SELECT v, sum(k) AS s FROM r GROUP BY v) b ON a.v = b.v LOOP END LOOP;
	FOR r IN EXPLAIN SELECT v, count(*), sum(k) FROM ordered WHERE k < 200 GROUP BY v LOOP END LOOP;
END $$;
RESET client_min_messages;
RESET sift.explain_refusals;

-- the heap check is of the rows that reach the groups: a lookup that keeps a seventh of them (v = 3) charges the pages that fewer rows touch
CREATE TABLE lookup (v int);
INSERT INTO lookup VALUES (3);
ANALYZE lookup;
CREATE TABLE lcosts AS
SELECT q, (SELECT scan_cost FROM sift.explain_why(q)) AS empty_cost
FROM unnest(ARRAY['SELECT o.v, count(*), sum(o.k) FROM ordered o GROUP BY o.v',
				  'SELECT o.v, count(*), sum(o.k) FROM ordered o JOIN lookup l ON l.v = o.v GROUP BY o.v']::text[]) q;

VACUUM ordered;
SELECT * FROM cte_why('WITH r AS (SELECT * FROM ordered WHERE k < 12) SELECT a.v, a.n, b.s FROM (SELECT v, count(*) AS n FROM r GROUP BY v) a JOIN (SELECT v, sum(k) AS s FROM r GROUP BY v) b ON a.v = b.v');
CREATE TABLE charges AS
SELECT k_below, c.empty_cost - (SELECT scan_cost FROM sift.explain_why(format('SELECT v, count(*), sum(k) FROM ordered WHERE k < %s GROUP BY v', k_below))) AS charge
FROM costs c;
SELECT (SELECT charge FROM charges WHERE k_below = 20) > 0 AS charged,
	(SELECT charge FROM charges WHERE k_below = 20) < 0.5 * (SELECT charge FROM charges WHERE k_below = 300) AS tenth_charges_less;
SELECT * FROM why('SELECT v, count(*), sum(k) FROM ordered WHERE k < 6 GROUP BY v');
SELECT used, slug, reason !~ 'all-visible' AS no_hint FROM sift.explain_why('SELECT count(*) FROM ordered WHERE k = 5');

SELECT (SELECT c.empty_cost - (SELECT scan_cost FROM sift.explain_why(c.q)) FROM lcosts c WHERE q ~ 'JOIN lookup')
	< 0.8 * (SELECT c.empty_cost - (SELECT scan_cost FROM sift.explain_why(c.q)) FROM lcosts c WHERE q !~ 'JOIN lookup') AS lookup_charges_less;
DROP FUNCTION why(text), cte_why(text);
DROP TABLE ordered, costs, charges, lookup, lcosts;
DROP EXTENSION pg_sift;
