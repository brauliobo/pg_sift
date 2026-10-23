-- ORDER BY a numeric of more than 18 digits LIMIT n: the top-N reads the key through the dictionary of its datums
-- label: `ORDER BY price LIMIT 10` over a `numeric(30,4)` column
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN price numeric(30,4);
UPDATE orders SET price = ((id::bigint * 7919) % 1000003)::numeric * 100000000000 + (id % 10000) / 10000.0;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, price)
--@ pg
SELECT price FROM orders ORDER BY price LIMIT 10
