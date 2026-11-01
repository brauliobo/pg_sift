-- the order array of a text column names its strings by dictionary codes: the size of the index and its build
-- label: `CREATE INDEX .. USING sift_col (id, title) WITH (order_array_columns = 'title')` over the rows of the run, distinct 26-byte strings; the b-tree over the same column in the other lane; the metric is the index size
--@ prepare
CREATE TABLE w AS SELECT g::int AS id, 'item-' || md5(g::text) AS title FROM generate_series(1, {rows}) g;
UPDATE w SET title = left(title, 26);
VACUUM (FREEZE, ANALYZE) w
--@ load
CREATE INDEX w_ix ON w (title)
--@ load_sift
CREATE INDEX w_ix ON w USING sift_col (id, title) WITH (order_array_columns = 'title')
--@ check
SELECT count(*) FROM w
