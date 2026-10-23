-- ORDER BY a uuid column LIMIT n: the keys are the codes of the dictionary the segment stores
-- label: `ORDER BY ref LIMIT 10` over a `uuid` column
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN ref uuid;
UPDATE orders SET ref = md5(id::text)::uuid;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, ref)
--@ pg
SELECT ref FROM orders ORDER BY ref LIMIT 10
