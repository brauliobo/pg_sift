-- a sum and an average of a numeric column without a precision whose values are almost all distinct are added from the lanes of the rows: an integer and a scale for each row, no dictionary of the unit's values and no numeric built
-- label: `sum` and `avg` of a `numeric` column without a precision, almost unique values, 6 groups
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN price_u numeric;
UPDATE orders SET price_u = ((id::bigint * 2654435761) % 100000007) / 100.0;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, status sift.col_text_ops, price_u)
--@ pg
SELECT status, count(*) AS n, sum(price_u) AS total, avg(price_u) AS mean FROM orders GROUP BY status ORDER BY status
