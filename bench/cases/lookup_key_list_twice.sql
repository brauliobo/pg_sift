-- a list of customers joined and again in an IN sub-select restricts one column once: the scan no longer wins at a price that counts the restriction twice, and the btree probes of the normal plan, which read the rows of the few customers and their flag, win over a scan that loads the whole table of flags
-- label: `WITH ids AS (6 customers)`, `orders JOIN ids ON ids.id = customer_id`, `customer_id IN (SELECT id FROM ids)`, `LEFT JOIN flags` (a wide row a customer, `flag IS NOT TRUE`), `sum(amount)` by customer and month, a btree on `(customer_id, created_at)`
-- ordered: yes
--@ setup
CREATE TABLE flags AS SELECT g AS id, g % 9 = 0 AS flag, repeat(md5(g::text), 56) AS pad FROM generate_series(1, (SELECT max(customer_id) FROM orders)) g;
ALTER TABLE flags ADD PRIMARY KEY (id);
CREATE INDEX orders_customer_created ON orders (customer_id, created_at);
ANALYZE flags;
--@ pg
WITH ids AS (SELECT id FROM (VALUES (11), (22), (33), (44), (55), (66)) v(id))
SELECT o.customer_id, date_trunc('month', o.created_at)::date AS month, sum(o.amount) AS revenue
FROM orders o JOIN ids ON ids.id = o.customer_id LEFT JOIN flags f ON f.id = o.customer_id
WHERE f.flag IS NOT TRUE AND o.customer_id IN (SELECT id FROM ids GROUP BY id) AND o.created_at >= date '2024-01-01' AND o.created_at < date '2025-01-01'
GROUP BY 1, 2 ORDER BY 1, 2
