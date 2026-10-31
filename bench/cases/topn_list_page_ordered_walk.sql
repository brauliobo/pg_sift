-- a page of a list (ORDER BY .. LIMIT 3) whose keys are a scope sub-select over two link tables and whose select list reads a column of the joined lookup table is an ordered walk with an early stop
-- label: `SELECT o.id, o.created_at, c.tier FROM orders o JOIN ol_customers c ON c.id = o.customer_id WHERE c.id IN (scope of 100 customers through links and groups) ORDER BY o.created_at DESC, o.id LIMIT 3`
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('ol_customers') IS NULL THEN
		CREATE TABLE ol_customers (id int PRIMARY KEY, tier text NOT NULL);
		INSERT INTO ol_customers SELECT g, 't' || g % 6 FROM generate_series(1, (SELECT max(customer_id) FROM orders)) g;
		CREATE TABLE ol_links (id int PRIMARY KEY, tenant_id int NOT NULL, group_id int, customer_id int);
		CREATE TABLE ol_group (group_id int NOT NULL, customer_id int NOT NULL);
		INSERT INTO ol_links SELECT g, 3, CASE WHEN g > 70 THEN g % 5 END, CASE WHEN g <= 70 THEN 1 + (g * 7919) % (SELECT max(id) FROM ol_customers) END FROM generate_series(1, 100) g;
		INSERT INTO ol_links SELECT 100 + g, 1 + g % 6, NULL, 1 + (g * 31) % (SELECT max(id) FROM ol_customers) FROM generate_series(1, 300) g;
		INSERT INTO ol_group SELECT g % 5, 1 + (g * 104729) % (SELECT max(id) FROM ol_customers) FROM generate_series(1, 150) g;
		ANALYZE ol_customers, ol_links, ol_group;
	END IF;
END $do$
--@ pg
SELECT o.id, o.created_at, c.tier FROM orders o JOIN ol_customers c ON c.id = o.customer_id
WHERE c.id IN (SELECT coalesce(l.customer_id, g.customer_id) FROM ol_links l LEFT JOIN ol_group g ON g.group_id = l.group_id WHERE l.tenant_id = 3)
ORDER BY o.created_at DESC, o.id LIMIT 3
