-- ORDER BY a text column of distinct values LIMIT n: the rows of a unit are compared with the bar by the order code of the dictionary
-- label: `ORDER BY sku COLLATE "C" LIMIT 100` over a `text` column of distinct values
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN sku text;
UPDATE orders SET sku = md5(id::text);
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, sku)
--@ pg
SELECT sku FROM orders ORDER BY sku COLLATE "C" LIMIT 100
