-- CREATE INDEX of a sift_col index: a parallel build launches a worker for each further 150,000 rows, so a table of a few hundred thousand rows is written in one or two segments, not one for each participant
-- label: `CREATE INDEX .. USING sift_col` over seven columns of a heap table with `key_cut = 'off'` (a key cut is built by one process); `index_mb` is the size of the index; the sift lane runs at the `min_parallel_table_scan_size` of the cluster of the run (1MB, which asks for its participants), the variant `server_default` at the default of PostgreSQL (8MB: a worker for each further 150,000 rows), `one_process` has no workers and `all_asked` asks for 4 whatever the rows; the native lane is a b-tree over the same columns
--@ prepare
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} ORDER BY id;
VACUUM (FREEZE, ANALYZE) w
--@ load
CREATE INDEX w_ix ON w (customer_id, country, category, status, amount, quantity, created_at)
--@ load_sift
CREATE INDEX w_ix ON w USING sift_col (customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = 'off')
--@ check
SELECT count(*) FROM w
