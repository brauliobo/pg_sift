-- the statement over 75 customers: the first statement of a backend over a sift_col index whose write buffer holds {rows}/8 rows, as the buffer is (every backend that reads it decodes its rows once) and once the reads have paid for its promotion (the worker seals and promotes it: regress col_wbuf_read_priced holds the trigger)
-- label: `count(*), sum(amount) GROUP BY status` of 75 customer_ids over {rows} rows of eight columns, `key_cut = customer_id`: seven eighths of the rows in the index, the rest in the write buffer; the sift lane reads the index after the promotion of the buffer, the variant `buffered` the same index with its buffer; the first statement of a new backend
-- fresh: yes
--@ setup
SELECT current_schema() AS main_schema \gset
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
DROP SCHEMA IF EXISTS c_col_wbuf_read_priced_set__buffered CASCADE;
CREATE SCHEMA c_col_wbuf_read_priced_set__buffered;
SET search_path = c_col_wbuf_read_priced_set__buffered, public;
DROP TABLE IF EXISTS w;
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} AND id % 8 <> 0 ORDER BY id;
CREATE INDEX w_col ON w USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = customer_id);
VACUUM (FREEZE, ANALYZE, INDEX_CLEANUP off) w;
INSERT INTO w SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} AND id % 8 = 0;
SET search_path = :"main_schema", public;
DROP TABLE IF EXISTS w;
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} AND id % 8 <> 0 ORDER BY id;
CREATE INDEX w_col ON w USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = customer_id);
VACUUM (FREEZE, ANALYZE, INDEX_CLEANUP off) w;
INSERT INTO w SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} AND id % 8 = 0;
SELECT sift.col_promote('w_col');
--@ pg
SELECT status, count(*), sum(amount) FROM w WHERE customer_id BETWEEN 100 AND 174 GROUP BY status
