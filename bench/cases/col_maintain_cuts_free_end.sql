-- the file of a sift_col index after churn and its maintenance: rounds of single-row UPDATE, DELETE and INSERT, each followed by VACUUM, then the rewrite of the segments, against a b-tree over the same columns
-- label: 4 rounds of 4,000 `UPDATE`s of an indexed column by id, 2,000 `DELETE`s and 2,000 `INSERT`s on a heap table of 200,000 orders with its primary key (4% of the rows a round), each followed by `VACUUM`; pg_sift: a `sift_col` index over seven columns, then `sift.col_maintain` (the rewrite); PostgreSQL: a b-tree over the same columns; `index_mb` is the size of that index at the end
--@ prepare
CREATE TABLE w (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING heap;
INSERT INTO w SELECT id, customer_id, country, category, status, amount, quantity, created_at, title FROM public.orders_src WHERE id <= 200000 ORDER BY id;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX w_ix ON w USING sift_col (customer_id, country, category, status, amount, quantity, created_at); ELSE CREATE INDEX w_ix ON w (customer_id, country, category, status, amount, quantity, created_at); END IF; END $$;
CREATE PROCEDURE churn(r int) LANGUAGE plpgsql AS $$ BEGIN FOR i IN 1..4000 LOOP UPDATE w SET amount = amount + 1 WHERE id = (i * 41 + r * 7) % 200000 + 1; COMMIT; END LOOP; FOR i IN 1..2000 LOOP DELETE FROM w WHERE id = (i * 97 + r * 13) % 200000 + 1; COMMIT; END LOOP; FOR i IN 1..2000 LOOP INSERT INTO w VALUES (10000000 + r * 10000 + i, i % 5000, 'US', 'toys', 'paid', 10.5, 1, timestamp '2025-06-01' + i * interval '1 second', 't'); COMMIT; END LOOP; END $$;
VACUUM ANALYZE w
--@ load
CALL churn(1);
VACUUM w;
CALL churn(2);
VACUUM w;
CALL churn(3);
VACUUM w;
CALL churn(4);
VACUUM w
--@ load_sift
CALL churn(1);
VACUUM w;
CALL churn(2);
VACUUM w;
CALL churn(3);
VACUUM w;
CALL churn(4);
VACUUM w;
SELECT sift.col_maintain('w_ix')
--@ check
SELECT count(*) FROM w
