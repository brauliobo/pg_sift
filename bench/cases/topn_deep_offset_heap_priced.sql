-- the heap of a deep page (OFFSET past 1,024 rows) is priced by the rows of the offset and the page, so the column scan is still chosen at an offset of 5,000
-- label: `SELECT id, created_at FROM ol_pages ORDER BY created_at DESC, id LIMIT 20 OFFSET 5000`; the variant `btree` is the same rows with a b-tree on (created_at, id)
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS topn_deep_offset_btree CASCADE;
CREATE SCHEMA topn_deep_offset_btree;
CREATE TABLE ol_pages WITH (autovacuum_enabled = false) AS SELECT id, created_at FROM orders;
CREATE INDEX ol_pages_col ON ol_pages USING sift_col (id, created_at);
CREATE TABLE topn_deep_offset_btree.ol_pages AS TABLE ol_pages;
CREATE INDEX ol_pages_bt ON topn_deep_offset_btree.ol_pages (created_at, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ol_pages;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) topn_deep_offset_btree.ol_pages
--@ pg
SELECT id, created_at FROM ol_pages ORDER BY created_at DESC, id LIMIT 20 OFFSET 5000
