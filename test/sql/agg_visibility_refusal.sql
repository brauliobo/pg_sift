-- A rewrite (CLUSTER, VACUUM FULL, pg_repack) leaves the visibility map of the table empty: the scan checks the heap for every row it keeps, the planner
-- prices that, and the normal plan wins until a VACUUM. sift.explain_why and sift.explain_refusals say so.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

-- rows in key order, seven to a page: a key range is a range of heap pages, and the btree answers the same range
CREATE TABLE ordered (id int, k int, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ordered SELECT i, i / 300, i % 7, repeat('x', 1000) FROM generate_series(0, 59999) i;
CREATE INDEX ordered_k ON ordered (k);
CREATE INDEX ordered_col ON ordered USING sift_col (id, k, v);
CLUSTER ordered USING ordered_k;
ANALYZE ordered;

CREATE FUNCTION why(q text) RETURNS TABLE (used bool, slug text, reason text) LANGUAGE sql AS $$
	SELECT used, slug, regexp_replace(reason, '[0-9]+(\.[0-9]+)?', 'N', 'g') FROM sift.explain_why(q)
$$;

-- what the empty map adds to the scan is bounded by the pages of the units the zones keep: a tenth of the table (k < 20 keeps 10% of the units)
-- charges about a tenth of its pages, the whole table (k < 300) about all of them
CREATE TABLE costs AS
SELECT k_below, (SELECT scan_cost FROM sift.explain_why(format('SELECT v, count(*), sum(k) FROM ordered WHERE k < %s GROUP BY v', k_below))) AS empty_cost
FROM unnest(ARRAY[20, 300]) k_below;

-- empty map: refused for its cost, with the pages that are not all-visible and the way out
SELECT * FROM why('SELECT v, count(*), sum(k) FROM ordered WHERE k < 40 GROUP BY v');
-- the share of the pages is the table's, not the query's
SELECT used, slug, reason ~ '^the normal plan is estimated cheaper than the scan \(the scan costs [0-9.]+, the normal plan [0-9.]+; 100% of the heap pages' AS pages_100
FROM sift.explain_why('SELECT v, count(*), sum(k) FROM ordered WHERE k < 40 GROUP BY v');
-- a scan that loses for another reason says nothing of the map: one key of a table the btree reads at once
SELECT used, slug, reason !~ 'all-visible' AS no_hint FROM sift.explain_why('SELECT count(*) FROM ordered WHERE k = 5');

VACUUM ordered;
SELECT k_below, (c.empty_cost - (SELECT scan_cost FROM sift.explain_why(format('SELECT v, count(*), sum(k) FROM ordered WHERE k < %s GROUP BY v', k_below))))
	/ (SELECT relpages FROM pg_class WHERE relname = 'ordered') < 0.5 AS under_half_the_pages
FROM costs c ORDER BY k_below;
SELECT * FROM why('SELECT v, count(*), sum(k) FROM ordered WHERE k < 40 GROUP BY v');
SELECT used, slug, reason !~ 'all-visible' AS no_hint FROM sift.explain_why('SELECT count(*) FROM ordered WHERE k = 5');

DROP FUNCTION why(text);
DROP TABLE ordered, costs;
DROP EXTENSION pg_sift;
