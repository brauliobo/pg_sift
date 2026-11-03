-- the build of a sift_col index of 13 columns: the cube, the order arrays and the key cut it chooses, against a b-tree over the same columns
-- label: `CREATE INDEX .. USING sift_col` over 13 columns of the rows of the run with the default options (the cube of each unit, `key_cut` and the order arrays the build chooses), with 0, 4 and 8 workers as variants; PostgreSQL builds a b-tree over the same 13 columns
--@ prepare
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at, (id % 97)::int AS a, (id % 1013)::int AS b, (id % 7)::smallint AS c, (id % 31)::int AS d, (id % 12 + 1)::int AS e, title FROM public.orders_src WHERE id <= {rows} ORDER BY id;
VACUUM (FREEZE, ANALYZE) w
--@ load
CREATE INDEX w_ix ON w (customer_id, country, category, status, amount, quantity, created_at, a, b, c, d, e, title)
--@ load_sift
CREATE INDEX w_ix ON w USING sift_col (customer_id, country, category, status, amount, quantity, created_at, a, b, c, d, e, title)
--@ check
SELECT count(*) FROM w
