-- the leaves of a jsonb column of a `sift_table` with declared paths (`sift.table_set_paths`): a group key and an aggregate over an expression of a column and a leaf (`qty * (doc ->> 'score')::float8`)
-- label: `doc ->> 'channel'`, `count(*)` and `sum(qty * (doc ->> 'score')::float8)` where `qty >= 3` of a sift_table of the sf_events documents; native reads a heap copy, the variant `gin` a heap copy with `GIN (doc jsonb_path_ops)`
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS tbl_j, tbl_j_heap;
CREATE TABLE tbl_j (id int NOT NULL, qty int NOT NULL, doc jsonb) USING sift_table WITH (autovacuum_enabled = false);
SELECT sift.table_set_paths('tbl_j', 'doc', 'doc->channel text, doc->score float8');
INSERT INTO tbl_j SELECT id, qty, doc FROM sf_events;
VACUUM (FREEZE, ANALYZE) tbl_j;
CREATE TABLE tbl_j_heap WITH (autovacuum_enabled = false) AS SELECT * FROM tbl_j;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) tbl_j_heap;
DROP SCHEMA IF EXISTS c_jsonb_sift_table_paths__gin CASCADE;
CREATE SCHEMA c_jsonb_sift_table_paths__gin;
CREATE TABLE c_jsonb_sift_table_paths__gin.tbl_j_heap WITH (autovacuum_enabled = false) AS SELECT * FROM tbl_j;
CREATE INDEX tbl_j_doc_gin ON c_jsonb_sift_table_paths__gin.tbl_j_heap USING gin (doc jsonb_path_ops);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_jsonb_sift_table_paths__gin.tbl_j_heap
--@ sift
SELECT doc ->> 'channel' AS channel, count(*) AS n, sum(qty * (doc ->> 'score')::float8) AS w FROM tbl_j WHERE qty >= 3 GROUP BY 1 ORDER BY 1
--@ pg
SELECT doc ->> 'channel' AS channel, count(*) AS n, sum(qty * (doc ->> 'score')::float8) AS w FROM tbl_j_heap WHERE qty >= 3 GROUP BY 1 ORDER BY 1
