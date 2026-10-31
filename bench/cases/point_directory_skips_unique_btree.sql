-- CREATE INDEX of a sift_col index over a table with a primary key: `point_tid_columns = 'auto'` leaves out the unique column the b-tree already answers
-- label: `CREATE INDEX` over the primary key and seven columns of a heap table; `index_mb` is the sift_col index (the primary key is left out); both sift lanes have `key_cut = 'off'` (a listed directory stops the automatic cut), the variant `listed_id` writes the directory of the key (`point_tid_columns = 'id'`) where the sift lane has `'auto'`, the native lane is a b-tree over the seven columns
--@ prepare
CREATE TABLE w (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp) USING heap;
INSERT INTO w SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src ORDER BY id;
VACUUM (FREEZE, ANALYZE) w
--@ load
CREATE INDEX w_ix ON w (customer_id, country, category, status, amount, quantity, created_at)
--@ load_sift
DO $$ BEGIN IF current_setting('application_name') = 'listed_id' THEN CREATE INDEX w_ix ON w USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = 'off', point_tid_columns = 'id'); ELSE CREATE INDEX w_ix ON w USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = 'off', point_tid_columns = 'auto'); END IF; END $$
--@ check
SELECT count(*) FROM w
