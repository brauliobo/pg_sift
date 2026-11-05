-- the lookup of a few customers in an index of partition-sized runs that each hold rows of every customer (a build that outgrew the memory of its segment writer): a unit is read in every run, until the rewrite cuts the runs by key range
-- ordered: yes
-- vacuum: no
-- label: `count(*), sum(amount) GROUP BY category WHERE customer_id IN (5 customers) AND title LIKE '%premium%'` over the rows of the run loaded in sixteen batches, each promoted into a segment of its own (`key_cut = 'customer_id'`, `sift.vacuum_rewrite_limit = 1` so that segments count as partitions, no VACUUM after the setup: it would merge the runs); the sift lane reads the runs, the variant `cut` the same index after `sift.col_maintain`
-- set: sift.col_auto_merge = off; sift.vacuum_rewrite_limit = 1
--@ setup
SET sift.col_auto_merge = off;
SET sift.vacuum_rewrite_limit = 1;
DROP TABLE IF EXISTS w;
CREATE TABLE w (id int, customer_id int, country text, category text, status text, amount numeric(10,2), created_at timestamp, title text) WITH (autovacuum_enabled = false);
CREATE INDEX w_col ON w USING sift_col (id, customer_id, country, category, status, amount, created_at, title) WITH (key_cut = 'customer_id');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 0 * ({rows} / 16) AND id <= 1 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 1 * ({rows} / 16) AND id <= 2 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 2 * ({rows} / 16) AND id <= 3 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 3 * ({rows} / 16) AND id <= 4 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 4 * ({rows} / 16) AND id <= 5 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 5 * ({rows} / 16) AND id <= 6 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 6 * ({rows} / 16) AND id <= 7 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 7 * ({rows} / 16) AND id <= 8 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 8 * ({rows} / 16) AND id <= 9 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 9 * ({rows} / 16) AND id <= 10 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 10 * ({rows} / 16) AND id <= 11 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 11 * ({rows} / 16) AND id <= 12 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 12 * ({rows} / 16) AND id <= 13 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 13 * ({rows} / 16) AND id <= 14 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 14 * ({rows} / 16) AND id <= 15 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
INSERT INTO w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 15 * ({rows} / 16) AND id <= 16 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('w_col');
VACUUM (FREEZE, ANALYZE) w;
DROP SCHEMA IF EXISTS c_key_cut_overlap_few_keys__cut CASCADE;
CREATE SCHEMA c_key_cut_overlap_few_keys__cut;
CREATE TABLE c_key_cut_overlap_few_keys__cut.w (LIKE w) WITH (autovacuum_enabled = false);
CREATE INDEX w_col ON c_key_cut_overlap_few_keys__cut.w USING sift_col (id, customer_id, country, category, status, amount, created_at, title) WITH (key_cut = 'customer_id');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 0 * ({rows} / 16) AND id <= 1 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 1 * ({rows} / 16) AND id <= 2 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 2 * ({rows} / 16) AND id <= 3 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 3 * ({rows} / 16) AND id <= 4 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 4 * ({rows} / 16) AND id <= 5 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 5 * ({rows} / 16) AND id <= 6 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 6 * ({rows} / 16) AND id <= 7 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 7 * ({rows} / 16) AND id <= 8 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 8 * ({rows} / 16) AND id <= 9 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 9 * ({rows} / 16) AND id <= 10 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 10 * ({rows} / 16) AND id <= 11 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 11 * ({rows} / 16) AND id <= 12 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 12 * ({rows} / 16) AND id <= 13 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 13 * ({rows} / 16) AND id <= 14 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 14 * ({rows} / 16) AND id <= 15 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
INSERT INTO c_key_cut_overlap_few_keys__cut.w SELECT id, customer_id, country, category, status, amount, created_at, title FROM public.orders_src WHERE id > 15 * ({rows} / 16) AND id <= 16 * ({rows} / 16) ORDER BY id;
SELECT sift.col_promote('c_key_cut_overlap_few_keys__cut.w_col');
SELECT sift.col_maintain('c_key_cut_overlap_few_keys__cut.w_col');
VACUUM (FREEZE, ANALYZE) c_key_cut_overlap_few_keys__cut.w
--@ pg
SELECT category, count(*) AS n, sum(amount) AS total FROM w WHERE customer_id IN (3, 40, 150, 700, 2500) AND title LIKE '%premium%' GROUP BY category ORDER BY category
