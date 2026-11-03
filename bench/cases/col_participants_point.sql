-- the statement over a sift_col index written by one process and by the participants of a parallel CREATE INDEX (segments of their own, interleaved): the floor of a statement grows with each segment
-- label: `count(*) and sum(amount) of one customer_id` over {rows} rows of seven columns, `key_cut = 'off'`; the sift lane reads the index built with no workers, the variant `parts` the same rows indexed by 4 workers whatever the rows (`min_parallel_table_scan_size = 0`)
--@ setup
DROP TABLE IF EXISTS w;
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} ORDER BY id;
SET max_parallel_maintenance_workers = 0;
CREATE INDEX w_col ON w USING sift_col (customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = 'off');
VACUUM (FREEZE, ANALYZE) w;
DROP SCHEMA IF EXISTS c_col_participants_point__parts CASCADE;
CREATE SCHEMA c_col_participants_point__parts;
CREATE TABLE c_col_participants_point__parts.w AS SELECT * FROM w ORDER BY id;
SET max_parallel_maintenance_workers = 4;
SET min_parallel_table_scan_size = 0;
CREATE INDEX w_col ON c_col_participants_point__parts.w USING sift_col (customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = 'off');
VACUUM (FREEZE, ANALYZE) c_col_participants_point__parts.w
--@ pg
SELECT count(*), sum(amount) FROM w WHERE customer_id = 17
