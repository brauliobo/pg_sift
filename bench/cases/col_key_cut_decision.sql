-- the choice of the default key_cut from a sample of the heap, after 4% churn
-- label: `sift.key_cut_decision` of a 7-column `sift_col` index after churn (the sample, the radix sort of the key and the order of the addresses of each unit) against `ANALYZE` of the same table in the other lane (PostgreSQL's own sample)
--@ prepare
CREATE TABLE w AS SELECT id, customer_id, country, category, status, amount, quantity, created_at, (id % 97)::int AS a, (id % 1013)::int AS b, (id % 7)::smallint AS c, (id % 31)::int AS d, (id % 12 + 1)::int AS e, title FROM public.orders_src WHERE id <= {rows} ORDER BY id;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX w_ix ON w USING sift_col (customer_id, country, category, status, amount, quantity, created_at); ELSE CREATE INDEX w_ix ON w (customer_id, country, category, status, amount, quantity, created_at); END IF; END $$;
VACUUM (FREEZE, ANALYZE) w;
UPDATE w SET amount = amount + 1 WHERE id % 50 = 1;
DELETE FROM w WHERE id % 100 = 3;
INSERT INTO w SELECT id + {rows}, customer_id, country, category, status, amount, quantity, created_at, a, b, c, d, e, title FROM w WHERE id % 100 = 5;
VACUUM w
--@ load
ANALYZE w
--@ load_sift
SELECT count(*) FROM sift.key_cut_decision('w_ix')
--@ check
SELECT count(*) FROM w
