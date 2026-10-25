-- the write overhead of a sift_col index on a heap table: 20,000 single-row UPDATEs of an indexed column by primary key, with the index (pg_sift) and without it (native)
-- label: 20,000 single-row `UPDATE w SET amount = amount + 1 WHERE id = <id>` on a heap table of 200,000 orders with its primary key; pg_sift: the table also has a `sift_col` index that covers `amount` (a heap table in both lanes, the lane only adds the index)
--@ prepare
CREATE TABLE w (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING heap;
INSERT INTO w SELECT id, customer_id, country, category, status, amount, quantity, created_at, title FROM public.orders_src WHERE id <= 200000 ORDER BY id;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX w_col ON w USING sift_col (customer_id, country, category, status, amount, quantity, created_at); END IF; END $$;
VACUUM ANALYZE w
--@ load
DO $$ BEGIN FOR i IN 1..20000 LOOP UPDATE w SET amount = amount + 1 WHERE id = i * 9 + 1; END LOOP; END $$
--@ check
SELECT count(*) FROM w
