-- sums of a smallint column in two integer operations ((w + k) * 3): the exact sum of the expression as one program for each row, not a transition function call for each row and sum (one operation, SUM(w + k), is the sums of the units: int2_sum_const)
-- label: `SUM((w + 1) * 3), .. SUM((w + 16) * 3)` of a `smallint` column
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN w smallint;
UPDATE orders SET w = 800 + (id::bigint * 7919) % 1700;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, status sift.col_text_ops, w)
--@ pg
SELECT SUM((w + 1) * 3), SUM((w + 2) * 3), SUM((w + 3) * 3), SUM((w + 4) * 3), SUM((w + 5) * 3), SUM((w + 6) * 3), SUM((w + 7) * 3), SUM((w + 8) * 3), SUM((w + 9) * 3), SUM((w + 10) * 3), SUM((w + 11) * 3), SUM((w + 12) * 3), SUM((w + 13) * 3), SUM((w + 14) * 3), SUM((w + 15) * 3), SUM((w + 16) * 3) FROM orders
