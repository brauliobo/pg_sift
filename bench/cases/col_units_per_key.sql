-- the statement over one key of a sift_col index that takes inserts all day: the segments the promotions leave over the keys of the first one (overlays), unmerged and merged by the rule of the maintenance
-- label: `count(*), sum(amount) GROUP BY status` of one customer_id over {rows} rows of eight columns, `key_cut = customer_id`: four fifths of the rows in the first segment, the rest promoted in 16 batches over the first 200 customers; the sift lane reads the index after 600 requests and `sift.col_maintain` (the overlays merged into one), the variant `layered` the same index with its 16 overlays
--@ setup
SELECT current_schema() AS main_schema \gset
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
CREATE FUNCTION c_col_units_per_key_requests(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  FOR i IN 1..n LOOP
    EXECUTE format('SELECT status, count(*), sum(amount) FROM w WHERE customer_id = %s GROUP BY status', 1 + (i * 7) % 200);
  END LOOP;
END $$;
DROP SCHEMA IF EXISTS c_col_units_per_key__layered CASCADE;
CREATE SCHEMA c_col_units_per_key__layered;
SET search_path = c_col_units_per_key__layered, public;
DROP TABLE IF EXISTS w;
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} AND id % 5 <> 0 ORDER BY id;
CREATE INDEX w_col ON w USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = customer_id);
SELECT s FROM generate_series(0, 15) i, LATERAL unnest(ARRAY[format('INSERT INTO w SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} AND id %% 5 = 0 AND customer_id <= 200 AND (id / 5) %% 40 = %s', i), 'SELECT sift.col_promote(''w_col'')']) WITH ORDINALITY u(s, o) ORDER BY i, o \gexec
VACUUM (FREEZE, ANALYZE, INDEX_CLEANUP off) w;
SET search_path = :"main_schema", public;
DROP TABLE IF EXISTS w;
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} AND id % 5 <> 0 ORDER BY id;
CREATE INDEX w_col ON w USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at) WITH (key_cut = customer_id);
SELECT s FROM generate_series(0, 15) i, LATERAL unnest(ARRAY[format('INSERT INTO w SELECT id, customer_id, country, category, status, amount, quantity, created_at FROM public.orders_src WHERE id <= {rows} AND id %% 5 = 0 AND customer_id <= 200 AND (id / 5) %% 40 = %s', i), 'SELECT sift.col_promote(''w_col'')']) WITH ORDINALITY u(s, o) ORDER BY i, o \gexec
VACUUM (FREEZE, ANALYZE, INDEX_CLEANUP off) w;
SELECT c_col_units_per_key_requests(600);
SELECT pg_stat_force_next_flush();
SELECT sift.col_maintain('w_col');
--@ pg
SELECT status, count(*), sum(amount) FROM w WHERE customer_id = 17 GROUP BY status
