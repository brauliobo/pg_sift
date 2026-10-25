-- a table with no b-tree and a sift_col index: 100 ids by IN list, which the planner reads from the index (sift.enable_col_scan on by default) and not by a sequential scan
-- label: `SELECT id, created_at WHERE id IN (100 ids)`, a `sift_col` index with the `sift.col_*_ops` classes and no b-tree; the native lane is the same table with `sift.enable_col_scan = off`
-- ordered: no
-- set: sift.enable_col_scan = on
--@ setup
CREATE TABLE orders_sc AS SELECT id, customer_id, created_at FROM orders;
CREATE INDEX orders_sc_col ON orders_sc USING sift_col (id sift.col_int4_ops, customer_id sift.col_int4_ops, created_at sift.col_timestamp_ops);
VACUUM (FREEZE, ANALYZE) orders_sc;
--@ pg
SELECT id, created_at FROM orders_sc WHERE id IN (1077,1154,1231,1308,1385,1462,1539,1616,1693,1770,1847,1924,2001,2078,2155,2232,2309,2386,2463,2540,2617,2694,2771,2848,2925,3002,3079,3156,3233,3310,3387,3464,3541,3618,3695,3772,3849,3926,4003,4080,4157,4234,4311,4388,4465,4542,4619,4696,4773,4850,4927,5004,5081,5158,5235,5312,5389,5466,5543,5620,5697,5774,5851,5928,6005,6082,6159,6236,6313,6390,6467,6544,6621,6698,6775,6852,6929,7006,7083,7160,7237,7314,7391,7468,7545,7622,7699,7776,7853,7930,8007,8084,8161,8238,8315,8392,8469,8546,8623,8700)
