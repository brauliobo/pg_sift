-- the grouped sum over the whole table of a key-cut index whose rows were updated and deleted since the last VACUUM: the tombstone log is asked once for a heap block and the units that list it again are answered from the scan
-- label: `count(*), sum(amount) GROUP BY country, status` over {rows} rows of eight columns, `key_cut = customer_id`, after an `UPDATE` of 5% and a `DELETE` of 5% of the rows with no `VACUUM` since (the log of the index holds them)
-- vacuum: no
--@ setup
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
DROP TABLE IF EXISTS lbc;
CREATE TABLE lbc WITH (autovacuum_enabled = false) AS SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} ORDER BY id;
ALTER TABLE lbc ADD PRIMARY KEY (id);
CREATE INDEX lbc_col ON lbc USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = customer_id);
VACUUM (FREEZE, ANALYZE) lbc;
UPDATE lbc SET amount = amount + 1 WHERE id % 20 = 1;
DELETE FROM lbc WHERE id % 20 = 2;
SELECT sift.col_promote('lbc_col');
ANALYZE lbc
--@ pg
SELECT country, status, count(*), sum(amount) FROM lbc GROUP BY country, status
