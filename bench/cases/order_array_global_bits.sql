-- the global orders of an index with order arrays only code their addresses with the bits of the largest offset of the chunk
-- label: `CREATE INDEX .. USING sift_col (id, k) WITH (order_columns = 'k', order_array_columns = 'k')` over a scattered integer (no point directory, no ends); the b-tree over the same column in the other lane; the metric is the index size
--@ prepare
CREATE TABLE w AS SELECT g::int AS id, (hashint4(g) & 1073741823) AS k FROM generate_series(1, {rows}) g ORDER BY md5((g * 31 + 7)::text);
VACUUM (FREEZE, ANALYZE) w
--@ load
CREATE INDEX w_ix ON w (k)
--@ load_sift
CREATE INDEX w_ix ON w USING sift_col (id, k) WITH (order_columns = 'k', order_array_columns = 'k')
--@ check
SELECT count(*) FROM w
