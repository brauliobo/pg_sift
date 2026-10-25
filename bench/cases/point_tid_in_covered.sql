-- an IN list of unique keys looked up in the point directory of heap addresses by a query the index covers: planned on the addresses and the heap, not through the index-only scan that reads every kept unit
-- label: `SELECT id, customer_id WHERE id IN (60 values)`, no btree on `id`, `point_tid_columns = 'id, customer_id'`, `sift.enable_col_scan`
-- ordered: no
-- set: sift.enable_col_scan = on
--@ setup
ALTER TABLE orders DROP CONSTRAINT orders_pkey;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, customer_id sift.col_int4_ops, amount sift.col_numeric_ops) WITH (point_tid_columns = 'id, customer_id')
--@ pg
SELECT id, customer_id FROM orders WHERE id IN (998,1995,2992,3989,4986,5983,6980,7977,8974,9971,10968,11965,12962,13959,14956,15953,16950,17947,18944,19941,20938,21935,22932,23929,24926,25923,26920,27917,28914,29911,30908,31905,32902,33899,34896,35893,36890,37887,38884,39881,40878,41875,42872,43869,44866,45863,46860,47857,48854,49851,50848,51845,52842,53839,54836,55833,56830,57827,58824,59821)
