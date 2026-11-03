-- CREATE INDEX of a sift_col index with the default options: the cost of a build in instructions of the leader and of the workers, blocks, WAL, bytes and the segments it writes
-- label: `CREATE INDEX .. USING sift_col` over six columns of a heap table with no option; the build record kind (the leader and the workers counted apart, the segments, the options); a b-tree over the same columns is the native lane
-- kind: build
--@ prepare
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, created_at FROM public.orders_src WHERE id <= {rows} ORDER BY id;
VACUUM (FREEZE, ANALYZE) w
--@ load
CREATE INDEX w_ix ON w (customer_id, country, category, status, amount, created_at)
--@ load_sift
CREATE INDEX w_ix ON w USING sift_col (customer_id, country, category, status, amount, created_at)
--@ check
SELECT count(*) FROM w
