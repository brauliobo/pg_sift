-- ORDER BY k LIMIT 20 OFFSET 10000 over a table stored in the order of k (12 columns, no order array on k): the 10,020 rows are read from the end of the one unit the key follows and the heap takes them as the run they already are
-- label: `SELECT * ORDER BY k LIMIT 20 OFFSET 10000` over {rows} rows of 12 columns stored in the order of k, the default index; the variant `btree` is the same rows with a b-tree on k
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS ops;
CREATE TABLE ops AS SELECT id AS k, customer_id, country, category, status, amount, quantity, created_at, (id % 97)::int AS a, (id % 1013)::int AS b, (id % 7)::int AS c, (id % 31)::int AS d FROM orders ORDER BY id;
CREATE INDEX ops_col ON ops USING sift_col (k, customer_id, country, category, status, amount, quantity, created_at, a, b, c, d);
VACUUM (FREEZE, ANALYZE) ops;
DROP SCHEMA IF EXISTS c_topn_offset_presorted__btree CASCADE;
CREATE SCHEMA c_topn_offset_presorted__btree;
CREATE TABLE c_topn_offset_presorted__btree.ops AS TABLE ops;
CREATE INDEX ops_bt ON c_topn_offset_presorted__btree.ops (k);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_topn_offset_presorted__btree.ops
--@ pg
SELECT * FROM ops ORDER BY k LIMIT 20 OFFSET 10000
