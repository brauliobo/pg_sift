-- a count of one text equality on a column of few strings: the dictionary holds no row counts there (count_columns leaves them off), so the groups of units (superunits) answer it where the scan read every unit and the unit heads between them
-- label: `SELECT count(*) WHERE label = '<one of 150 values>'`, 150 strings spread over the table, a one-column `sift_col` index; the variant `units` is the same scan with `sift.agg_super = off` (the units are read, as before the groups answered it)
-- ordered: no
--@ setup
CREATE TABLE orders_lb AS SELECT id, 'v' || (hashint8extended(id, 3) & 2147483647) % 150 AS label FROM orders;
CREATE INDEX orders_lb_col ON orders_lb USING sift_col (label);
VACUUM (FREEZE, ANALYZE) orders_lb
--@ pg
SELECT count(*) AS n FROM orders_lb WHERE label = 'v77'
