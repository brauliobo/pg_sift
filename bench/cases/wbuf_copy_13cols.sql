-- a row written to the write buffer of a sift_col index by COPY is encoded and queued without a call of the StringInfo functions for each value
-- label: `COPY` of a tenth of the rows of the run (13 columns) into a table of the rows of the run with a 13-column `sift_col` index; a b-tree over the same columns in the other lane; the metrics are the time and the instructions of the COPY
--@ prepare
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at, (id % 97)::int AS a, (id % 1013)::int AS b, (id % 7)::smallint AS c, (id % 31)::int AS d, (id % 12 + 1)::int AS e FROM public.orders_src WHERE id <= {rows} ORDER BY id;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX w_ix ON w USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at, a, b, c, d, e); ELSE CREATE INDEX w_ix ON w (id, customer_id, country, category, status, amount, quantity, created_at, a, b, c, d, e); END IF; END $$;
VACUUM (FREEZE, ANALYZE) w;
COPY (SELECT * FROM w WHERE id % 10 = 0) TO '/dev/shm/sift_bench_wbuf_copy_13cols.csv' (FORMAT csv)
--@ load
COPY w FROM '/dev/shm/sift_bench_wbuf_copy_13cols.csv' (FORMAT csv)
--@ check
SELECT count(*) FROM w
