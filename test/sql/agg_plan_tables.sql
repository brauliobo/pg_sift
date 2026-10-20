-- The right side of a lookup join of Custom Scan (SiftAgg) can be a plan: a sub-select PostgreSQL keeps (GROUP BY, LIMIT, DISTINCT), a VALUES list or a
-- function. Its rows are those of PostgreSQL's own plan of it, run once when the scan starts, so the result is the one of the normal plan
-- (docs/AGG.md, "Joins"). Every statement is run with the pushdown and with sift.enable_agg = off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- the table is small: page costs that keep the btree and bitmap plans from looking cheap as well
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, entity_id int, store_id int, region text, kind text, amount numeric(10, 2), memo text) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 300 END, 1 + i % 40, (ARRAY['north', 'south', 'east', 'west'])[1 + i % 4],
       (ARRAY['a', 'b', 'c'])[1 + i % 3], (i % 500) / 10.0, (ARRAY['clean', 'broken', 'fast', 'late'])[1 + i % 4] || ' ' || (ARRAY['box', 'wifi', 'label'])[1 + i % 3]
FROM generate_series(1, 6000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, entity_id, store_id, region, kind, amount);
CREATE INDEX ev_fts ON ev USING sift (memo);
CREATE TABLE stores (id int PRIMARY KEY, kind text, country text);
INSERT INTO stores SELECT i, (ARRAY['outlet', 'popup', 'flagship'])[1 + i % 3], (ARRAY['north', 'south', 'east', NULL])[1 + i % 4] FROM generate_series(1, 40) i;
VACUUM (FREEZE, ANALYZE) ev;
ANALYZE stores;

-- the shape of the plan: SiftAgg with the sub-plan under it, or the normal plan
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; j text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%Lookup Joins:%' THEN j := ' ' || trim(split_part(l, ':', 2)); END IF;
		IF l LIKE '%Semi Lookup Tables:%' THEN j := j || ', semi table'; END IF;
		IF l LIKE '%Workers Launched: _%' AND l NOT LIKE '%Workers Launched: 0%' THEN r := r || ' with workers'; END IF;
	END LOOP;
	RETURN r || j;
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- VALUES: a repeated key joins once per row of the list, a NULL key joins nothing, LEFT keeps what has no row once
SELECT * FROM same($$SELECT m.label, count(*) AS n, sum(amount) AS s FROM ev e JOIN (VALUES ('north', 'N'), ('north', 'Nord'), ('east', 'E'), (NULL, 'none')) m(region, label) ON e.region = m.region GROUP BY m.label$$);
SELECT * FROM same($$SELECT m.label, count(*) AS n, count(m.label) AS labelled FROM ev e LEFT JOIN (VALUES ('north', 'N'), ('north', 'Nord'), ('east', 'E'), (NULL, 'none')) m(region, label) ON e.region = m.region GROUP BY m.label$$);
SELECT * FROM same($$SELECT m.band, count(*) AS n, sum(amount * m.f) AS s FROM ev e JOIN (VALUES (1, 'a', 1.5), (2, 'a', 2.0), (2, 'b', 0.5), (NULL, 'z', 9.0)) m(store_id, band, f) ON e.store_id = m.store_id GROUP BY m.band$$);
-- IN (VALUES ...) is a semi lookup join before PostgreSQL 18 and a = ANY array from 18: the join list is not part of what the line proves
SELECT regexp_replace(plan, 'values \(semi\), ', '') AS plan, result_rows, vs_normal_plan FROM same($$SELECT count(*) AS n FROM ev WHERE store_id IN (VALUES (1), (2), (2), (3), (NULL)) AND NOT EXISTS (SELECT 1 FROM (VALUES (2), (4)) v(id) WHERE v.id = ev.store_id)$$);
-- a sub-select with GROUP BY (a NULL group has a row that matches nothing), LIMIT, DISTINCT and a set operation
SELECT * FROM same($$SELECT c.stores, count(*) AS n FROM ev e JOIN (SELECT country, count(*) AS stores FROM stores GROUP BY country) c ON e.region = c.country GROUP BY c.stores$$);
SELECT * FROM same($$SELECT c.stores, count(*) AS n FROM ev e JOIN (SELECT kind, count(*) AS stores FROM stores GROUP BY kind) c ON e.kind = c.kind GROUP BY c.stores$$);
SELECT * FROM same($$SELECT s.kind, count(*) AS n, sum(amount) AS s FROM ev e JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 5) s ON e.store_id = s.id GROUP BY s.kind$$);
SELECT * FROM same($$SELECT s.kind, count(*) AS n FROM ev e LEFT JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 12) s ON e.store_id = s.id GROUP BY s.kind$$);
SELECT * FROM same($$SELECT s.kind, count(*) AS n FROM ev e JOIN (SELECT DISTINCT id, kind FROM stores WHERE id % 2 = 0 OFFSET 0) s ON e.store_id = s.id GROUP BY s.kind$$);
SELECT * FROM same($$SELECT s.id, count(*) AS n FROM ev e JOIN (SELECT id FROM stores WHERE id < 5 UNION SELECT id FROM stores WHERE id < 8) s ON e.store_id = s.id GROUP BY s.id$$);
-- a condition on a column of the sub-select that nobody else reads is pushed into it: the sub-plan outputs a NULL there, the normal plan stays
SELECT * FROM same($$SELECT e.kind, count(*) AS n FROM ev e JOIN (SELECT entity_id, count(*) AS n FROM ev GROUP BY entity_id) s ON e.entity_id = s.entity_id WHERE s.n > 19 GROUP BY e.kind$$);
SELECT * FROM same($$SELECT e.kind, max(s.n) AS busiest, count(*) AS n FROM ev e JOIN (SELECT entity_id, count(*) AS n FROM ev GROUP BY entity_id) s ON e.entity_id = s.entity_id WHERE s.n > 19 GROUP BY e.kind$$);
-- the table is the fact table and the sub-select (a child with a scan of its own), and the sub-select groups a column with NULLs
SELECT * FROM same($$SELECT e.kind, count(*) AS n, max(s.n) AS busiest FROM ev e JOIN (SELECT entity_id, count(*) AS n FROM ev GROUP BY entity_id HAVING count(*) > 20) s ON e.entity_id = s.entity_id GROUP BY e.kind$$);
SELECT * FROM same($$SELECT e.kind, s.n, count(*) AS c FROM ev e LEFT JOIN (SELECT entity_id, count(*) AS n FROM ev GROUP BY entity_id) s ON e.entity_id = s.entity_id GROUP BY e.kind, s.n$$);
-- a function: generate_series
SELECT * FROM same($$SELECT g.n % 3 AS b, count(*) AS n FROM ev e JOIN generate_series(1, 30) AS g(n) ON e.store_id = g.n GROUP BY 1$$);
SELECT * FROM same($$SELECT count(*) AS n FROM ev e WHERE e.store_id IN (SELECT g FROM generate_series(1, 10) g)$$);
-- no rows in the child, and key types that differ from the leaves (bigint against int, varchar against text)
SELECT * FROM same($$SELECT s.kind, count(*) AS n FROM ev e JOIN (SELECT id, kind FROM stores WHERE id < 0 LIMIT 3) s ON e.store_id = s.id GROUP BY s.kind$$);
SELECT * FROM same($$SELECT s.kind, count(*) AS n FROM ev e LEFT JOIN (SELECT id, kind FROM stores WHERE id < 0 LIMIT 3) s ON e.store_id = s.id GROUP BY s.kind$$);
SELECT * FROM same($$SELECT s.kind, count(*) AS n FROM ev e JOIN (SELECT id::bigint AS id, kind FROM stores WHERE id < 30 OFFSET 0) s ON e.store_id = s.id GROUP BY s.kind$$);
SELECT * FROM same($$SELECT s.kind, count(*) AS n FROM ev e JOIN (SELECT region::varchar AS region, 'x' AS kind FROM ev GROUP BY region) s ON e.region = s.region GROUP BY s.kind$$);

-- an aggregate over a ranked top-k: IN a BM25 scan ordered by score (the SiftScan stays the child), and the scan as a join with its score
SELECT * FROM same($$SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE id IN (SELECT id FROM ev WHERE memo @@> 'clean' ORDER BY sift.score(ctid) DESC, id LIMIT 100) GROUP BY kind$$);
SELECT * FROM same($$SELECT e.kind, count(*) AS n, max(r.score) AS best FROM ev e JOIN (SELECT id, sift.score(ctid) AS score FROM ev WHERE memo @@> 'clean wifi' ORDER BY sift.score(ctid) DESC, id LIMIT 50) r ON e.id = r.id GROUP BY e.kind$$);

-- the plan: the sub-plan is the child of the node, in EXPLAIN as well; the keys of a sub-select with LIMIT are a byte table (semi join)
SELECT plan_of($$SELECT m.label, count(*) FROM ev e JOIN (VALUES ('north', 'N'), ('east', 'E')) m(region, label) ON e.region = m.region GROUP BY m.label$$) AS values_join;
SELECT plan_of($$SELECT s.kind, count(*) FROM ev e JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 5) s ON e.store_id = s.id GROUP BY s.kind$$) AS limit_join;
SELECT plan_of($$SELECT count(*) FROM ev WHERE store_id IN (SELECT id FROM stores ORDER BY id LIMIT 5)$$) AS semi_limit;
SELECT plan_of($$SELECT g.n % 2, count(*) FROM ev e JOIN generate_series(1, 5) AS g(n) ON e.store_id = g.n GROUP BY 1$$) AS function_join;
EXPLAIN (COSTS OFF) SELECT s.kind, count(*) FROM ev e JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 5) s ON e.store_id = s.id GROUP BY s.kind;
EXPLAIN (COSTS OFF) SELECT count(*) FROM ev WHERE id IN (SELECT id FROM ev WHERE memo @@> 'clean' ORDER BY sift.score(ctid) DESC LIMIT 100);

-- what keeps the normal plan: a volatile side, a side that reads the order (LATERAL), ON true, or a side above sift.agg_lookup_max_rows.
-- A nullable fact table joined to a sub-select also keeps the normal plan (the preserved side: docs/AGG.md).
SELECT plan_of($$SELECT m.label, count(*) FROM ev e JOIN (VALUES ('north', 'N', random() < 2)) m(region, label, ok) ON e.region = m.region AND m.ok GROUP BY m.label$$) AS volatile_values;
SELECT plan_of($$SELECT s.kind, count(*) FROM ev e JOIN (SELECT id, kind FROM stores WHERE random() < 2 LIMIT 100) s ON e.store_id = s.id GROUP BY s.kind$$) AS volatile_subselect;
SELECT plan_of($$SELECT k.kind, count(*) FROM ev e JOIN LATERAL (SELECT s.kind FROM stores s WHERE s.id = e.store_id ORDER BY s.id LIMIT 1) k ON k.kind IS NOT NULL GROUP BY k.kind$$) AS lateral_subselect;
SELECT plan_of($$SELECT s.kind, count(*) FROM ev e JOIN (SELECT kind FROM stores ORDER BY id LIMIT 1) s ON true GROUP BY s.kind$$) AS on_true;
-- A predicate-only child join is exact for every match, as for an equality join.
SELECT * FROM same($$SELECT s.kind, count(*) AS n FROM ev e JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 30) s ON e.store_id < s.id GROUP BY s.kind$$);
SELECT plan_of($$SELECT s.id, count(e.id) FROM (SELECT id FROM stores ORDER BY id LIMIT 5) s LEFT JOIN ev e ON e.store_id = s.id GROUP BY s.id$$) AS fact_nullable;
SET sift.agg_lookup_max_rows = 10;
SELECT plan_of($$SELECT s.kind, count(*) FROM ev e JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 30) s ON e.store_id = s.id GROUP BY s.kind$$) AS above_cap;
SELECT plan_of($$SELECT s.kind, count(*) FROM ev e JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 5) s ON e.store_id = s.id GROUP BY s.kind$$) AS under_cap;
RESET sift.agg_lookup_max_rows;
SET sift.enable_agg = off;
SELECT plan_of($$SELECT s.kind, count(*) FROM ev e JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 5) s ON e.store_id = s.id GROUP BY s.kind$$) AS pushdown_off;
RESET sift.enable_agg;
-- parallel workers: a child plan is the leader's, the node asks for none
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SELECT * FROM same($$SELECT m.label, count(*) AS n, sum(amount) AS s FROM ev e JOIN (VALUES ('north', 'N'), ('east', 'E')) m(region, label) ON e.region = m.region GROUP BY m.label$$);
RESET max_parallel_workers_per_gather; RESET parallel_setup_cost; RESET parallel_tuple_cost; RESET min_parallel_table_scan_size;

-- a rescan: the node is the inner side of a nested loop and runs again for each row of the outer side (the groups are replayed), twice in a CTE
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;
SELECT * FROM same($$SELECT g, c.n FROM generate_series(1, 3) g CROSS JOIN (SELECT count(*) AS n FROM ev e JOIN (VALUES (1), (2), (2)) v(id) ON e.store_id = v.id) c$$);
SELECT * FROM same($$WITH c AS NOT MATERIALIZED (SELECT s.kind, count(*) AS n FROM ev e JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 9) s ON e.store_id = s.id GROUP BY s.kind)
	SELECT a.kind, a.n, b.n AS again FROM c a JOIN c b ON a.kind = b.kind$$);
RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_material;

-- parameters in the sub-select: a custom plan and a generic one read them like the normal plan
PREPARE lim(int, text) AS SELECT s.kind, count(*) AS n, sum(amount) AS s FROM ev e JOIN (SELECT id, kind FROM stores WHERE kind <> $2 ORDER BY id LIMIT $1) s ON e.store_id = s.id GROUP BY s.kind ORDER BY 1;
SET plan_cache_mode = force_custom_plan;
EXECUTE lim(7, 'outlet');
EXECUTE lim(20, 'popup');
SET plan_cache_mode = force_generic_plan;
EXECUTE lim(7, 'outlet');
EXECUTE lim(20, 'popup');
SET sift.enable_agg = off;
EXECUTE lim(7, 'outlet');
EXECUTE lim(20, 'popup');
RESET sift.enable_agg; RESET plan_cache_mode;

-- a cursor reads the snapshot it was declared in: the sub-select and the scan both, whatever commits (here the transaction's own later changes) afterwards
CREATE TABLE tags (id int, label text);
INSERT INTO tags VALUES (1, 'one'), (2, 'two'), (3, 'three');
BEGIN;
DECLARE c_on CURSOR FOR SELECT t.label, count(*) AS n FROM ev e JOIN (SELECT id, label FROM tags ORDER BY id LIMIT 10) t ON e.store_id = t.id GROUP BY t.label ORDER BY 1;
SET LOCAL sift.enable_agg = off;
DECLARE c_off CURSOR FOR SELECT t.label, count(*) AS n FROM ev e JOIN (SELECT id, label FROM tags ORDER BY id LIMIT 10) t ON e.store_id = t.id GROUP BY t.label ORDER BY 1;
RESET sift.enable_agg;
DELETE FROM tags WHERE id = 2;
INSERT INTO tags VALUES (4, 'four');
FETCH ALL FROM c_on;
FETCH ALL FROM c_off;
CLOSE c_on; CLOSE c_off;
ROLLBACK;

DROP FUNCTION same(text);
DROP FUNCTION plan_of(text);
DROP TABLE ev, stores, tags;
DROP EXTENSION pg_sift;
