-- A filtered GROUP BY on a table that CLUSTER has just rewritten (empty visibility map, no VACUUM yet)
-- label: `count(*), sum(amount) WHERE created_at in a quarter GROUP BY country` after `CLUSTER`
-- ordered: yes
-- vacuum: no
--@ setup
CLUSTER orders USING orders_created;
ANALYZE orders
--@ pg
SELECT country, count(*), sum(amount) FROM orders WHERE created_at >= '2025-04-01' AND created_at < '2025-07-01' GROUP BY country ORDER BY country
