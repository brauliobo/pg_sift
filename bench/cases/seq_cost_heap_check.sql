-- the heap check of a scan priced as the sequential scan PostgreSQL would run, with seq_page_cost above random_page_cost (100 and 4)
-- label: `count(*), sum(amount) WHERE created_at in a quarter GROUP BY country` after `CLUSTER`, `seq_page_cost = 100`, `random_page_cost = 4`
-- ordered: yes
-- vacuum: no
-- set: seq_page_cost = 100; random_page_cost = 4
--@ setup
CLUSTER orders USING orders_created;
ANALYZE orders
--@ pg
SELECT country, count(*), sum(amount) FROM orders WHERE created_at >= '2025-04-01' AND created_at < '2025-07-01' GROUP BY country ORDER BY country
