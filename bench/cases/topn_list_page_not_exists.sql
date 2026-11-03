-- a page of a list that leaves out the customers of a scope with NOT EXISTS is an ordered walk: the keys of the sub-select are a NOT IN conjunct of the scan
-- label: `SELECT o.id, o.created_at FROM ol_orders o WHERE o.customer_id IN (20 customers) AND NOT EXISTS (SELECT 1 FROM ol_links l WHERE l.tenant_id = 3 AND l.customer_id = o.customer_id) ORDER BY o.created_at, o.id LIMIT 3`; the variant `btree` is the same rows with a b-tree on (created_at, id)
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS topn_not_exists_btree CASCADE;
CREATE SCHEMA topn_not_exists_btree;
DO $do$ BEGIN
	IF to_regclass('ol_links') IS NULL THEN
		CREATE TABLE ol_links (id int PRIMARY KEY, tenant_id int NOT NULL, group_id int, customer_id int);
		INSERT INTO ol_links SELECT g, 3, NULL, 1 + (g * 7919) % (SELECT max(customer_id) FROM orders) FROM generate_series(1, 100) g;
		ANALYZE ol_links;
	END IF;
END $do$;
CREATE TABLE ol_orders WITH (autovacuum_enabled = false) AS SELECT id, customer_id, created_at FROM orders;
ALTER TABLE ol_orders ALTER COLUMN customer_id SET NOT NULL;
CREATE INDEX ol_orders_col ON ol_orders USING sift_col (id, customer_id, created_at);
CREATE TABLE topn_not_exists_btree.ol_orders AS TABLE ol_orders;
ALTER TABLE topn_not_exists_btree.ol_orders ALTER COLUMN customer_id SET NOT NULL;
CREATE INDEX ol_orders_bt ON topn_not_exists_btree.ol_orders (created_at, id);
CREATE TABLE topn_not_exists_btree.ol_links AS TABLE ol_links;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ol_orders;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) topn_not_exists_btree.ol_orders;
ANALYZE topn_not_exists_btree.ol_links
--@ pg
SELECT o.id, o.created_at FROM ol_orders o
WHERE o.customer_id IN (12001, 12002, 12003, 12004, 12005, 12006, 12007, 12008, 12009, 12010, 12011, 12012, 12013, 12014, 12015, 12016, 12017, 12018, 12019, 12020)
  AND NOT EXISTS (SELECT 1 FROM ol_links l WHERE l.tenant_id = 3 AND l.customer_id = o.customer_id)
ORDER BY o.created_at, o.id LIMIT 3
