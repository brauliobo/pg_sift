-- a text equality on distinct short keys whose order array keeps its strings: the hash table and the sorted order of the dictionary are made once for every backend by the first statement that needs them
-- label: `count(*)` where a text column of one distinct 9-character key for each row, with an order array, equals one of them, the first statement of a new backend
-- ordered: yes
-- fresh: yes
--@ setup
ALTER TABLE orders ADD COLUMN tag text;
UPDATE orders SET tag = 'k' || lpad(id::text, 8, '0');
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, amount, tag) WITH (order_array_columns = 'tag')
--@ pg
SELECT count(*), sum(amount) FROM orders WHERE tag = 'k00000777'
