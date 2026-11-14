-- The parents that have a child, counted by the planner as the distinct keys of the children that have a parent (agg/flat/invert.c), and the aggregates of a parent left-joined to its
-- children made for each key of the children first (agg/flat/partial.c): the rows are those of the normal plan whatever happens to either table, and a parent key that is not unique, or a
-- shape that is not a count, keeps the statement as written.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

-- the answer under the agg scan and under the normal plan, and whether the child was counted by its keys (the distinct keys of the children, or, for the parents that lack a child, the scan of the children that keeps the parents nothing matched)
CREATE FUNCTION invert_run(q text, OUT same boolean, OUT inverted boolean)
LANGUAGE plpgsql AS $$
DECLARE
	p text;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	inverted := p LIKE '%count(DISTINCT%' OR p LIKE '%"o (subquery) (left)%' OR (p LIKE '%(preserved)%' AND p LIKE '%IS NULL%');
END $$;

CREATE TABLE accounts (id int PRIMARY KEY, region int, name text) WITH (autovacuum_enabled = false);
INSERT INTO accounts SELECT i, i % 7, 'a' || i FROM generate_series(1, 3000) i;
CREATE INDEX accounts_col ON accounts USING sift_col (id, region, name);
-- no foreign key: keys the parent has no row for (99999) and NULL keys
CREATE TABLE orders (id int PRIMARY KEY, account_id int, v int, flag boolean) WITH (autovacuum_enabled = false);
INSERT INTO orders SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 99999 ELSE 1 + (i * 7) % 2800 END, i % 10, i % 3 = 0 FROM generate_series(1, 120000) i;
CREATE INDEX orders_col ON orders USING sift_col (id, account_id, v, flag) WITH (connect = 'account_id -> accounts(id)');
-- the keys of the parent are not unique
CREATE TABLE loose (id int, region int) WITH (autovacuum_enabled = false);
INSERT INTO loose SELECT 1 + i % 100, i % 5 FROM generate_series(1, 300) i;
CREATE INDEX loose_col ON loose USING sift_col (id, region);
CREATE TABLE loose_orders (id int PRIMARY KEY, loose_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO loose_orders SELECT i, 1 + (i * 3) % 120, i % 10 FROM generate_series(1, 2000) i;
CREATE INDEX loose_orders_col ON loose_orders USING sift_col (id, loose_id, v);
-- bigint and smallint keys
CREATE TABLE big_parent (id int8 PRIMARY KEY, region int) WITH (autovacuum_enabled = false);
INSERT INTO big_parent SELECT i, i % 4 FROM generate_series(1, 20000) i;
CREATE INDEX big_parent_col ON big_parent USING sift_col (id, region);
CREATE TABLE big_child (id int PRIMARY KEY, big_parent_id int8, v int) WITH (autovacuum_enabled = false);
INSERT INTO big_child SELECT i, 1 + (i * 11) % 23000, i % 10 FROM generate_series(1, 80000) i;
CREATE INDEX big_child_col ON big_child USING sift_col (id, big_parent_id, v);
CREATE TABLE small_parent (id int2 PRIMARY KEY, region int) WITH (autovacuum_enabled = false);
INSERT INTO small_parent SELECT i, i % 4 FROM generate_series(1, 20000) i;
CREATE INDEX small_parent_col ON small_parent USING sift_col (id, region);
CREATE TABLE small_child (id int PRIMARY KEY, small_parent_id int2, v int) WITH (autovacuum_enabled = false);
INSERT INTO small_child SELECT i, 1 + (i * 13) % 23000, i % 10 FROM generate_series(1, 80000) i;
CREATE INDEX small_child_col ON small_child USING sift_col (id, small_parent_id, v);
VACUUM (FREEZE, ANALYZE) accounts, orders, loose, loose_orders, big_parent, big_child, small_parent, small_child;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT count(*) FROM accounts a WHERE EXISTS (SELECT 1 FROM orders o WHERE o.account_id = a.id AND o.v < 3)$q$),
 (2, $q$SELECT count(*) FROM accounts a WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.account_id = a.id AND o.v < 3)$q$),
 (3, $q$SELECT count(*) FROM accounts a WHERE EXISTS (SELECT 1 FROM orders o WHERE o.account_id = a.id)$q$),
 (4, $q$SELECT count(*) FROM accounts a WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.account_id = a.id)$q$),
 (5, $q$SELECT count(*) FROM accounts a WHERE a.region = 3 AND EXISTS (SELECT 1 FROM orders o WHERE o.account_id = a.id AND o.flag)$q$),
 (6, $q$SELECT count(*) FROM accounts a WHERE a.region < 3 AND NOT EXISTS (SELECT 1 FROM orders o WHERE o.account_id = a.id AND o.flag AND o.v > 4)$q$),
 (7, $q$SELECT count(*) FROM accounts a WHERE EXISTS (SELECT 1 FROM orders o WHERE a.id = o.account_id AND o.v = ANY (ARRAY[1, 2]))$q$),
 (8, $q$SELECT count(*) FROM big_parent p WHERE EXISTS (SELECT 1 FROM big_child c WHERE c.big_parent_id = p.id AND c.v < 4)$q$),
 (9, $q$SELECT count(*) FROM small_parent p WHERE NOT EXISTS (SELECT 1 FROM small_child c WHERE c.small_parent_id = p.id AND c.v < 4)$q$),
 (10, $q$SELECT count(*) FROM loose p WHERE EXISTS (SELECT 1 FROM loose_orders c WHERE c.loose_id = p.id AND c.v < 4)$q$),
 (11, $q$SELECT count(*) FROM accounts a WHERE EXISTS (SELECT 1 FROM orders o WHERE o.account_id = a.id AND o.v < 3) OR a.region = 1$q$),
 (12, $q$SELECT count(*), 1 FROM accounts a WHERE EXISTS (SELECT 1 FROM orders o WHERE o.account_id = a.id)$q$),
 (13, $q$SELECT count(*) FROM accounts a WHERE EXISTS (SELECT 1 FROM orders o WHERE o.account_id = a.id AND o.v < a.region)$q$),
 (14, $q$SELECT a.region, count(o.id), sum(o.v), min(o.v), max(o.id) FROM accounts a LEFT JOIN orders o ON o.account_id = a.id GROUP BY a.region ORDER BY 1$q$),
 (15, $q$SELECT a.region, count(*), count(o.flag), sum(o.v) FROM accounts a LEFT JOIN orders o ON o.account_id = a.id AND o.v < 5 WHERE a.id > 100 GROUP BY a.region ORDER BY 1$q$),
 (16, $q$SELECT a.id, count(o.id) FROM accounts a LEFT JOIN orders o ON o.account_id = a.id GROUP BY a.id ORDER BY 2 DESC, 1 LIMIT 10$q$);
CREATE FUNCTION phase() RETURNS TABLE (n int, same boolean, inverted boolean) LANGUAGE sql AS $$
	SELECT s.n, r.same, r.inverted FROM shapes s, LATERAL invert_run(s.sql) r ORDER BY s.n
$$;

-- stamped segments
SELECT sift.col_maintain('orders_col') AS rewritten;
SELECT * FROM phase();

-- children written after the stamp: keys the parent lacks, one it has a row for later, a key changed, rows deleted
INSERT INTO orders SELECT 200000 + i, 3001 + i, 1, true FROM generate_series(0, 19) i;
UPDATE orders SET account_id = 5 WHERE id % 50 = 1;
UPDATE orders SET v = 9 WHERE id % 30 = 2;
DELETE FROM orders WHERE id % 40 = 3;
SELECT * FROM phase();

-- parents: one inserted for a key the children hold, one deleted, a key changed, a row updated in place
INSERT INTO accounts VALUES (3001, 1, 'late');
DELETE FROM accounts WHERE id IN (10, 11);
UPDATE accounts SET id = 4000 WHERE id = 12;
UPDATE accounts SET name = 'changed', region = 3 WHERE id BETWEEN 20 AND 40;
SELECT * FROM phase();

-- rows in the write buffer, then merged
INSERT INTO orders SELECT 300000 + i, 2900 + i, 2, false FROM generate_series(0, 49) i;
SELECT * FROM phase();
SELECT sift.col_maintain('orders_col') AS rewritten;
SELECT * FROM phase();
VACUUM accounts, orders;
SELECT * FROM phase();

-- the same under parallel workers
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT * FROM phase();
INSERT INTO orders SELECT 310000 + i, 100 + i, 3, true FROM generate_series(0, 9) i;
DELETE FROM accounts WHERE id BETWEEN 100 AND 110;
SELECT * FROM phase();

DROP TABLE orders, accounts, loose_orders, loose, big_child, big_parent, small_child, small_parent, shapes CASCADE;
DROP FUNCTION invert_run(text), phase();
DROP EXTENSION pg_sift;
