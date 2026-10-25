-- a sum of a product of three decimal(15,2) columns (47 digits, more than 128 bits) is added in 256 bits with the number of wraps, not evaluated by numeric_mul for each row
-- label: `sum(price * (1 - disc) * (1 + tax))`, `sum(price * (1 - disc))` and `avg(price)` of three `decimal(15,2)` columns, 6 groups, 1,000,000 rows
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN price numeric(15,2), ADD COLUMN disc numeric(15,2), ADD COLUMN tax numeric(15,2);
UPDATE orders SET price = 900 + ((id::bigint * 7919) % 100000), disc = (id % 11) / 100.0, tax = (id % 9) / 100.0;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, status sift.col_text_ops, price, disc, tax)
--@ pg
SELECT status, sum(price) AS base, sum(price * (1 - disc)) AS disc_price, sum(price * (1 - disc) * (1 + tax)) AS charge, avg(price) AS mean, count(*) AS n FROM orders GROUP BY status ORDER BY status
