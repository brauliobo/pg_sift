-- VACUUM of a table with a 13-column sift_col index after 1% of the rows were deleted, against the same VACUUM with a b-tree
-- label: `VACUUM` after `DELETE` of 1% of the rows of the run: the zones and the sums of the chunks of a 13-column `sift_col` index (a b-tree over the same columns in the other lane)
--@ prepare
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at, (id % 97)::int AS a, (id % 1013)::int AS b, (id % 7)::smallint AS c, (id % 31)::int AS d, (id % 12 + 1)::int AS e, title FROM public.orders_src WHERE id <= {rows} ORDER BY id;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX w_ix ON w USING sift_col (customer_id, country, category, status, amount, quantity, created_at, a, b, c, d, e, title); ELSE CREATE INDEX w_ix ON w (customer_id, country, category, status, amount, quantity, created_at, a, b, c, d, e, title); END IF; END $$;
VACUUM (FREEZE, ANALYZE) w;
DELETE FROM w WHERE id % 100 = 3
--@ load
VACUUM w
--@ check
SELECT count(*) FROM w
