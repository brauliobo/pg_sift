-- a count of one text equality on a column of few strings: the dictionary holds no row counts there (count_columns leaves them off), so the groups of units (superunits) answer it where the scan read every unit
-- label: `SELECT count(*) WHERE label = '<one of 150 values>'`, 150 strings spread over the table, a `sift_col` index on the column; the variant `units` is the same scan with `sift.agg_super = off`
-- ordered: no
--@ setup
ALTER TABLE orders ADD COLUMN label text;
UPDATE orders SET label = 'v' || (hashint8extended(id, 3) & 2147483647) % 150;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, label);
VACUUM (FREEZE, ANALYZE) orders
--@ pg
SELECT count(*) AS n FROM orders WHERE label = 'v77'
