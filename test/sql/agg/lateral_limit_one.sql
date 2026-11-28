-- A LATERAL sub-select of one table cut to one row (LIMIT 1) is the dimension of a join on Format 2.0, also inside an EXISTS over the elements of an array: each statement runs with the cost settings of the coverage proofs (a page costs 10000,
-- no Memoize) and with sift.enable_agg = off, and the rows must be the same. The engine column is the plan of the first run: SiftAggV6 for a sub-select the node reads whole, normal for one
-- whose rows depend on the row it is joined to (an OFFSET, an inequality, a join) or that is not the first of a table.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET TimeZone = 'UTC';
-- the library of the testing build creates the indexes of format major 1 by default; these are of format major 2, which the release library creates
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

CREATE TABLE o (id int PRIMARY KEY, tenant int, grp int, status text, total int, x int, y text) WITH (autovacuum_enabled = false);
INSERT INTO o
SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE 1 + i % 60 END, i % 8, CASE WHEN i % 13 = 0 THEN NULL ELSE 's' || (i % 5) END, i % 97, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE 'k' || (i % 4) END
FROM generate_series(1, 4000) i;
CREATE INDEX o_col ON o USING sift_col (id, tenant, grp, status, total, x, y);
VACUUM (FREEZE, ANALYZE) o;

-- a unique key: keys with no row (41..60), a NULL payload
CREATE TABLE u (id int PRIMARY KEY, kind text, rank int);
INSERT INTO u SELECT i, CASE WHEN i % 6 = 0 THEN NULL ELSE 'kind' || i % 4 END, i % 5 FROM generate_series(1, 40) i;
-- a key with ties: the rows of a key differ in their payload, in the order of the table (the first one is the one the normal plan returns), a NULL key, a NULL payload
CREATE TABLE tie (k int, v text, rnk int, flag boolean);
INSERT INTO tie SELECT 1 + i % 8, 'v' || i, i % 3, i % 4 <> 0 FROM generate_series(1, 64) i;
INSERT INTO tie VALUES (NULL, 'null key', 0, true), (3, NULL, 9, true);
-- the same rows with an index on the key and a few of them rewritten, so that the order of the table is not the order they were inserted in
CREATE TABLE tie_ix AS SELECT * FROM tie;
CREATE INDEX tie_ix_k ON tie_ix (k);
UPDATE tie_ix SET v = v || 'u' WHERE k IN (2, 5) AND rnk = 0;
-- a key of two columns, text keys
CREATE TABLE pair (a int, b text, w int);
INSERT INTO pair SELECT i % 7, 'k' || (i % 4), i FROM generate_series(1, 50) i;
-- a table with no row
CREATE TABLE gone (id int, v text);
-- the documents of an EXISTS over their elements: elements that pass more than once, none, NULL values, an empty array, a NULL document
CREATE TABLE d (id int PRIMARY KEY, tenant int, total int, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO d
SELECT i, CASE WHEN i % 19 = 0 THEN NULL ELSE 1 + i % 50 END, i % 97,
       CASE WHEN i % 23 = 0 THEN NULL WHEN i % 17 = 0 THEN '[]'::jsonb ELSE
       jsonb_build_array(jsonb_build_object('category', 'c' || (i % 5), 'kind', CASE WHEN i % 7 = 0 THEN NULL ELSE 'k' || (i % 3) END, 'qty', CASE WHEN i % 11 = 0 THEN NULL ELSE i % 6 END),
                         jsonb_build_object('category', 'c' || (i % 4), 'kind', 'k' || (i % 2), 'qty', i % 3),
                         jsonb_build_object('category', 'c' || (i % 5), 'kind', 'k' || (i % 3), 'qty', 2)) END
FROM generate_series(1, 3000) i;
CREATE INDEX d_col ON d USING sift_col (id, tenant, total, items) WITH (paths = 'items[]{category text, kind text, qty int4}');
VACUUM (FREEZE, ANALYZE) d;
-- the lookup of an item: a key with two rows (the first is the one that counts), NULL keys, a NULL mapped category, keys with no row
CREATE TABLE mp (store_kind text, category text, kind text, derived text);
INSERT INTO mp VALUES ('kind1', 'c1', 'k1', 'x'), ('kind1', 'c1', 'k1', 'y'), ('kind2', 'c2', 'k2', 'y'), ('kind3', 'c0', 'k0', NULL), (NULL, 'c3', 'k1', 'x'), ('kind0', 'c4', NULL, 'y'),
	('kind1', 'c0', 'k0', 'z'), ('kind2', 'c1', 'k1', 'x'), ('kind0', 'c3', 'k2', 'z'), ('kind3', 'c4', 'k1', 'y'), ('kind1', 'c2', 'k0', 'x'), ('kind2', 'c0', 'k2', 'y');
-- a table that holds a key more than once
CREATE TABLE ut (id int, kind text);
INSERT INTO ut SELECT i % 30, 'kind' || (i % 4) FROM generate_series(1, 60) i;
ANALYZE mp; ANALYZE ut;
ANALYZE u; ANALYZE tie; ANALYZE tie_ix; ANALYZE pair; ANALYZE gone;

CREATE FUNCTION same(q text) RETURNS TABLE (engine text, result_rows bigint, vs_heap bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	engine := 'normal';
	SET LOCAL seq_page_cost = 10000;
	SET LOCAL random_page_cost = 10000;
	SET LOCAL enable_memoize = off;
	SET LOCAL enable_tidscan = off;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAggV6)%' THEN engine := 'SiftAggV6'; END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_heap := (SELECT count(*) FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE stmts (id int, q text);
INSERT INTO stmts VALUES
-- a unique key: left and inner, keys with no row, NULL keys, a NULL payload
(1, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant LIMIT 1) s ON true GROUP BY s.kind'),
(2, 'SELECT s.kind, count(*) FROM o JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant LIMIT 1) s ON true GROUP BY s.kind'),
(3, 'SELECT s.kind, s.rank, count(*), sum(o.total) FROM o CROSS JOIN LATERAL (SELECT kind, rank FROM u WHERE u.id = o.tenant LIMIT 1) s GROUP BY s.kind, s.rank'),
(4, 'SELECT count(s.kind), count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant LIMIT 1) s ON true'),
(5, 'SELECT o.status, s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant LIMIT 1) s ON true WHERE o.total < 50 GROUP BY o.status, s.kind'),
-- the order of the sub-select breaks the ties of a unique key as well as of a repeated one
(6, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant ORDER BY rank DESC LIMIT 1) s ON true GROUP BY s.kind'),
-- a key with ties and no order: the first row of the key in the table; with an order on the payload
(7, 'SELECT s.v, count(*) FROM o LEFT JOIN LATERAL (SELECT v FROM tie WHERE tie.k = o.grp + 1 LIMIT 1) s ON true GROUP BY s.v'),
(8, 'SELECT s.v, count(*) FROM o JOIN LATERAL (SELECT v FROM tie WHERE tie.k = o.grp + 1 LIMIT 1) s ON true GROUP BY s.v'),
(9, 'SELECT s.rnk, count(*) FROM o LEFT JOIN LATERAL (SELECT rnk FROM tie WHERE tie.k = o.grp + 1 ORDER BY rnk DESC LIMIT 1) s ON true GROUP BY s.rnk'),
(10, 'SELECT s.rnk, s.v, count(*) FROM o LEFT JOIN LATERAL (SELECT rnk, v FROM tie WHERE tie.k = o.grp + 1 ORDER BY rnk DESC, v LIMIT 1) s ON true GROUP BY s.rnk, s.v'),
(11, 'SELECT s.rnk, count(*) FROM o LEFT JOIN LATERAL (SELECT rnk FROM tie WHERE tie.k = o.grp + 1 AND tie.flag ORDER BY rnk NULLS FIRST LIMIT 1) s ON true GROUP BY s.rnk'),
-- the first row of a key by an index on it, after rows were rewritten
(12, 'SELECT s.v, count(*) FROM o LEFT JOIN LATERAL (SELECT v FROM tie_ix WHERE tie_ix.k = o.grp + 1 LIMIT 1) s ON true GROUP BY s.v'),
(13, 'SELECT s.v, count(*) FROM o JOIN LATERAL (SELECT v FROM tie_ix WHERE tie_ix.k = o.grp + 1 AND tie_ix.flag LIMIT 1) s ON true GROUP BY s.v'),
-- keys of two columns, a text key, an expression of the fact row as the key
(14, 'SELECT s.w, count(*) FROM o LEFT JOIN LATERAL (SELECT w FROM pair WHERE pair.a = o.x AND pair.b = o.y ORDER BY w LIMIT 1) s ON true GROUP BY s.w'),
(15, 'SELECT s.w, count(*) FROM o JOIN LATERAL (SELECT w FROM pair WHERE pair.b = o.y AND pair.a = o.x LIMIT 1) s ON true GROUP BY s.w'),
(16, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant + o.x - 3 LIMIT 1) s ON true GROUP BY s.kind'),
-- the table has no row, or the conditions of the table alone leave none
(17, 'SELECT s.v, count(*) FROM o LEFT JOIN LATERAL (SELECT v FROM gone WHERE gone.id = o.tenant LIMIT 1) s ON true GROUP BY s.v'),
(18, 'SELECT s.v, count(*) FROM o JOIN LATERAL (SELECT v FROM gone WHERE gone.id = o.tenant LIMIT 1) s ON true GROUP BY s.v'),
(19, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant AND u.rank > 7 LIMIT 1) s ON true GROUP BY s.kind'),
(20, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant AND u.kind IS NOT NULL LIMIT 1) s ON true GROUP BY s.kind'),
-- with another dimension: the key of the sub-select is a column of it, and a plain join before the sub-select
(21, 'SELECT u.kind, s.v, count(*) FROM o JOIN u ON u.id = o.tenant LEFT JOIN LATERAL (SELECT v FROM tie WHERE tie.k = u.rank + 1 LIMIT 1) s ON true GROUP BY u.kind, s.v'),
(22, 'SELECT u.kind, s.rnk, count(*) FROM o LEFT JOIN u ON u.id = o.tenant JOIN LATERAL (SELECT rnk FROM tie WHERE tie.k = o.grp + 1 ORDER BY rnk LIMIT 1) s ON true GROUP BY u.kind, s.rnk'),
-- two sub-selects
(23, 'SELECT s.kind, t.v, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant LIMIT 1) s ON true LEFT JOIN LATERAL (SELECT v FROM tie WHERE tie.k = o.grp + 1 LIMIT 1) t ON true GROUP BY s.kind, t.v'),
-- aggregates over the columns of the sub-select, the filter of the fact side, a HAVING
(24, 'SELECT o.grp, sum(s.rank), min(s.kind), count(DISTINCT s.kind) FROM o LEFT JOIN LATERAL (SELECT kind, rank FROM u WHERE u.id = o.tenant LIMIT 1) s ON true WHERE o.status IS NOT NULL GROUP BY o.grp HAVING sum(s.rank) > 10'),
-- a second condition on the row of the fact table is a key of the sub-select too
(25, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant AND u.rank = o.x LIMIT 1) s ON true GROUP BY s.kind'),
-- a condition of the query on the columns of the sub-select: it applies to the row the sub-select keeps for a key, not to the rows it chose from
(26, 'SELECT s.kind, count(*) FROM o JOIN LATERAL (SELECT kind, rank FROM u WHERE u.id = o.tenant LIMIT 1) s ON true WHERE s.rank = 2 GROUP BY s.kind'),
(27, 'SELECT s.rank, count(*) FROM o JOIN LATERAL (SELECT kind, rank FROM u WHERE u.id = o.tenant ORDER BY rank DESC LIMIT 1) s ON true WHERE s.rank > 2 AND s.kind IS NOT NULL GROUP BY s.rank'),
(28, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant LIMIT 1) s ON true WHERE s.kind IS NULL OR s.kind = ''kind2'' GROUP BY s.kind'),
-- an EXISTS over the items with a lookup of one row for each item, by the kind of the store and by the category and kind of the item (a column of a joined table and two of the item)
(29, 'SELECT u.kind, count(*), count(DISTINCT d.id), sum(d.total) FROM d JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM LATERAL (SELECT coalesce(m.derived, i.category) AS category FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.store_kind = u.kind AND mp.category = i.category AND mp.kind = i.kind AND mp.derived IS NOT NULL LIMIT 1) m ON true WHERE i.qty IS NOT NULL) x WHERE x.category IN (''x'', ''c2'')) GROUP BY u.kind'),
(30, 'SELECT u.kind, count(*), sum(d.total) FROM d JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM LATERAL (SELECT coalesce(m.derived, i.category) AS category FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.store_kind = u.kind AND mp.category = i.category AND mp.kind = i.kind LIMIT 1) m ON true) x WHERE x.category = ''y'') GROUP BY u.kind'),
(31, 'SELECT count(*), sum(d.total) FROM d JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.category = i.category AND mp.kind = i.kind LIMIT 1) m ON true WHERE m.derived = ''z'')'),
(32, 'SELECT u.kind, count(*) FROM d JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.store_kind = u.kind AND mp.category = i.category LIMIT 1) m ON true WHERE m.derived IS NULL AND i.qty = 2) GROUP BY u.kind'),
-- the same row passes by more than one item: it counts once, and a filter of the row and of the joined table comes with it
(33, 'SELECT u.kind, count(*), sum(d.total), min(d.id), max(d.id) FROM d JOIN u ON u.id = d.tenant WHERE d.total < 60 AND u.rank > 0 AND EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) WHERE i.qty >= 0 AND i.category <> ''c9'') GROUP BY u.kind'),
(34, 'SELECT u.rank, count(*) FROM d LEFT JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.category = i.category AND mp.kind = i.kind LIMIT 1) m ON true WHERE m.derived IS NOT NULL OR i.qty = 0) GROUP BY u.rank'),
-- two lookups, the second keyed by the first
(35, 'SELECT u.kind, count(*) FROM d JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.category = i.category AND mp.kind = i.kind LIMIT 1) m ON true LEFT JOIN LATERAL (SELECT mp2.derived AS d2 FROM mp mp2 WHERE mp2.category = m.derived LIMIT 1) m2 ON true WHERE coalesce(m2.d2, m.derived, i.category) = ''y'') GROUP BY u.kind'),
-- no row, no item, nothing the lookup finds
(36, 'SELECT count(*) FROM d JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.category = upper(i.category) LIMIT 1) m ON true WHERE m.derived IS NOT NULL)'),
(37, 'SELECT count(*) FROM d JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) WHERE i.qty > 100)'),
-- the normal plan: NOT EXISTS, an OR, a joined table that holds a key more than once (two EXISTS beside the join are two tests of the scan)
(38, 'SELECT u.kind, count(*) FROM d JOIN u ON u.id = d.tenant WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.category = i.category LIMIT 1) m ON true WHERE m.derived = ''x'') GROUP BY u.kind'),
(39, 'SELECT u.kind, count(*) FROM d JOIN u ON u.id = d.tenant WHERE d.total = 5 OR EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.category = i.category LIMIT 1) m ON true WHERE m.derived = ''x'') GROUP BY u.kind'),
(40, 'SELECT u.kind, count(*) FROM d JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) WHERE i.qty = 1) AND EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) WHERE i.qty = 2) GROUP BY u.kind'),
(41, 'SELECT ut.kind, count(*) FROM d JOIN ut ON ut.id = d.tenant WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.category = i.category LIMIT 1) m ON true WHERE m.derived = ''x'') GROUP BY ut.kind'),
-- a sub-select whose rows depend on the row it is joined to, that is not one table, or not the first row of it: the normal plan
(42, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant OFFSET 0 LIMIT 1) s ON true GROUP BY s.kind'),
(43, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id <= o.tenant ORDER BY id DESC LIMIT 1) s ON true GROUP BY s.kind'),
(44, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant LIMIT 2) s ON true GROUP BY s.kind'),
(45, 'SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT u.kind FROM u JOIN tie ON tie.k = u.id WHERE u.id = o.tenant LIMIT 1) s ON true GROUP BY s.kind');

SELECT s.id, r.* FROM stmts s, LATERAL same(s.q) r ORDER BY s.id;

-- the plan of a statement: the node reads the dimension once, as the plan of the query PostgreSQL makes of it
CREATE FUNCTION plan_of(q text, opts text DEFAULT 'COSTS OFF') RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	SET LOCAL seq_page_cost = 10000;
	SET LOCAL random_page_cost = 10000;
	SET LOCAL enable_memoize = off;
	FOR l IN EXECUTE 'EXPLAIN (' || opts || ') ' || q LOOP
		RETURN NEXT regexp_replace(l, '\s+$', '');
	END LOOP;
END $$;
SELECT * FROM plan_of('SELECT s.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM u WHERE u.id = o.tenant LIMIT 1) s ON true GROUP BY s.kind') WHERE plan_of ~ 'Custom Scan|Scan on|Unique|Sort';
SELECT * FROM plan_of('SELECT s.v, count(*) FROM o LEFT JOIN LATERAL (SELECT v FROM tie WHERE tie.k = o.grp + 1 LIMIT 1) s ON true GROUP BY s.v') WHERE plan_of ~ 'Custom Scan|Scan on|Unique|Sort';
-- an EXISTS over the items with a lookup for each item: the elements are the unnest of the node, the lookup is its dimension, and no sub-plan is left under it
SELECT * FROM plan_of('SELECT u.kind, count(*), count(DISTINCT d.id), sum(d.total) FROM d JOIN u ON u.id = d.tenant WHERE EXISTS (SELECT 1 FROM LATERAL (SELECT coalesce(m.derived, i.category) AS category FROM jsonb_to_recordset(d.items) AS i(category text, kind text, qty int) LEFT JOIN LATERAL (SELECT mp.derived FROM mp WHERE mp.store_kind = u.kind AND mp.category = i.category AND mp.kind = i.kind AND mp.derived IS NOT NULL LIMIT 1) m ON true WHERE i.qty IS NOT NULL) x WHERE x.category IN (''x'', ''c2'')) GROUP BY u.kind') WHERE plan_of ~ 'Custom Scan|Unnest|Scan on|Unique|Sort Key|SubPlan';
-- the same under EXPLAIN VERBOSE: the columns of the dimension are named
SELECT count(*) FILTER (WHERE plan_of ~ 'Custom Scan \(SiftAggV6\)') AS nodes FROM plan_of('SELECT s.v, count(*) FROM o LEFT JOIN LATERAL (SELECT v FROM tie WHERE tie.k = o.grp + 1 LIMIT 1) s ON true GROUP BY s.v', 'VERBOSE, COSTS OFF');

DROP FUNCTION same(text);
DROP FUNCTION plan_of(text, text);
DROP TABLE stmts, o, u, tie, tie_ix, pair, gone, d, mp, ut;
DROP EXTENSION pg_sift;
