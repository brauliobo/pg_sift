-- a sum and an average of a numeric column without a precision (stored as an image, read through a dictionary of the unit's values) are added from the entries of the dictionary: an integer and a scale decoded once for each entry, no numeric built for a row
-- label: `sum` and `avg` of a `numeric` column without a precision, 100,000 distinct values, 6 groups
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN amount_w numeric;
UPDATE orders SET amount_w = ((id::bigint * 7919) % 100000) / 100.0;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, status sift.col_text_ops, amount_w)
--@ pg
SELECT status, count(*) AS n, sum(amount_w) AS total, avg(amount_w) AS mean FROM orders GROUP BY status ORDER BY status
