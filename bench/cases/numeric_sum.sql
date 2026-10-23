-- a sum over a numeric column without a precision: the scan is priced for the dictionary entries it reads as text
-- label: `sum(price)` over a `numeric` column with no precision, `WHERE country = 'US'`
--@ setup
ALTER TABLE orders ADD COLUMN price numeric;
UPDATE orders SET price = amount;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, price)
--@ pg
SELECT country, sum(price) FROM orders GROUP BY country ORDER BY country
