-- the keys of an IN list are looked up in the point directory and each unit goes on as the rows it lists, where the scan decoded the key column of every unit
-- label: `customer_id IN (51 values)`, `count`, `sum(amount)`, no btree on `customer_id`, `point_tid_columns = 'customer_id'`
-- ordered: yes
--@ setup
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, quantity, created_at, (date_trunc('month', created_at)::date)) WITH (point_tid_columns = 'customer_id')
--@ pg
SELECT count(*), sum(amount) FROM orders WHERE customer_id IN (1, 7920, 15839, 23758, 31677, 39596, 7515, 15434, 23353, 31272, 39191, 7110, 15029, 22948, 30867, 38786, 6705, 14624, 22543, 30462, 38381, 6300, 14219, 22138, 30057, 37976, 5895, 13814, 21733, 29652, 37571, 5490, 13409, 21328, 29247, 37166, 5085, 13004, 20923, 28842, 36761, 4680, 12599, 20518, 28437, 36356, 4275, 12194, 20113, 28032, 35951)
