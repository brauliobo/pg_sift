-- keys with many rows each (a status holds 5 to 55% of the table) are not looked up in the point directory: the zones and summaries answer the list
-- label: `status IN ('paid', 'shipped')`, `count`, `sum(amount)`, `point_columns = 'status'`, no btree on `status`
-- ordered: yes
--@ setup
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, quantity, created_at, (date_trunc('month', created_at)::date)) WITH (point_columns = 'status')
--@ pg
SELECT count(*), sum(amount) FROM orders WHERE status IN ('paid', 'shipped')
