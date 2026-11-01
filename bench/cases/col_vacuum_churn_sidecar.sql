-- VACUUM after 4% churn of a table with a 7-column sift_col index: the sidecar of every unit, against a b-tree
-- label: `VACUUM` after an `UPDATE` of 2%, a `DELETE` of 1% and an `INSERT` of 1% of the rows of the run (7 columns, a sidecar for every unit: the cube takes the minimum and maximum of each group)
--@ prepare
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at, (id % 97)::int AS a, (id % 1013)::int AS b, (id % 7)::smallint AS c, (id % 31)::int AS d, (id % 12 + 1)::int AS e, title FROM public.orders_src WHERE id <= {rows} ORDER BY id;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX w_ix ON w USING sift_col (customer_id, country, category, status, amount, quantity, created_at); ELSE CREATE INDEX w_ix ON w (customer_id, country, category, status, amount, quantity, created_at); END IF; END $$;
VACUUM (FREEZE, ANALYZE) w;
UPDATE w SET amount = amount + 1 WHERE id % 50 = 1;
DELETE FROM w WHERE id % 100 = 3;
INSERT INTO w SELECT id + {rows}, customer_id, country, category, status, amount, quantity, created_at, a, b, c, d, e, title FROM w WHERE id % 100 = 5
--@ load
VACUUM w
--@ check
SELECT count(*) FROM w
