-- two sift_col indexes on a table, the newer cut by a key: a count over a month is answered by the older one, the index cut by block range, not by the newest
-- label: `count(*), sum(amount) WHERE created_at` in one month, a `sift_col` index with `key_cut = 'customer_id'` created after the default one
-- ordered: yes
--@ setup
CREATE INDEX orders_kc ON orders USING sift_col (customer_id, created_at, amount) WITH (key_cut = 'customer_id')
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE created_at >= timestamp '2025-03-01' AND created_at < timestamp '2025-04-01'
