-- the entries a deep OFFSET throws away are not fetched from the heap in an index cut by a key (`key_cut`), so the page is read from the sorted array of the column
-- label: `SELECT id, created_at FROM ok_pages ORDER BY created_at LIMIT 20 OFFSET 5000` over an index with `key_cut = 'customer_id'`; the variant `btree` is the same rows with a b-tree on (created_at)
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS topn_offset_key_cut_btree CASCADE;
CREATE SCHEMA topn_offset_key_cut_btree;
CREATE TABLE ok_pages WITH (autovacuum_enabled = false) AS SELECT id, customer_id, created_at FROM orders;
CREATE INDEX ok_pages_col ON ok_pages USING sift_col (id, customer_id, created_at) WITH (key_cut = 'customer_id');
CREATE TABLE topn_offset_key_cut_btree.ok_pages AS TABLE ok_pages;
CREATE INDEX ok_pages_bt ON topn_offset_key_cut_btree.ok_pages (created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ok_pages;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) topn_offset_key_cut_btree.ok_pages
--@ pg
SELECT id, created_at FROM ok_pages ORDER BY created_at LIMIT 20 OFFSET 5000
