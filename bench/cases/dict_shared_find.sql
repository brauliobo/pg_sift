-- the first statement of a backend that tests one value of a text column of 100,000 values of 37 bytes: the hash table another backend made is read where it lies in shared memory, not built again
-- label: `count(*)` where a text column with 100,000 distinct values equals one of them, the first statement of a new backend
-- ordered: yes
-- fresh: yes
--@ setup
ALTER TABLE orders ADD COLUMN tag text;
UPDATE orders SET tag = 'item-' || md5(((id::bigint * 7919) % 100000)::text);
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, amount, tag)
--@ pg
SELECT count(*), sum(amount) FROM orders WHERE tag = 'item-' || md5('777')
