-- a grouped count and sum over a sift_table with 10% of its rows updated, 5% inserted and 5% deleted since the last VACUUM: the tombstones of a page of a column group are read once for the page
-- label: `SELECT country, count(*), sum(amount) ... GROUP BY country` over a `sift_table` of the orders with 10% updated, 5% inserted and 5% deleted and no VACUUM after, against a heap table with the same churn
--@ prepare
CREATE TABLE w (id bigint, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING {am};
INSERT INTO w SELECT id, customer_id, country, category, status, amount, quantity, created_at, title FROM public.orders_src WHERE id <= {rows} ORDER BY id;
ALTER TABLE w ADD PRIMARY KEY (id);
VACUUM w;
UPDATE w SET amount = amount + 1 WHERE id % 10 = 0;
INSERT INTO w SELECT id + 100000000, customer_id, country, category, status, amount, quantity, created_at, title FROM w WHERE id % 20 = 1;
DELETE FROM w WHERE id % 20 = 2;
ANALYZE w
--@ load
SELECT country, count(*), sum(amount) FROM w GROUP BY country
--@ check
SELECT string_agg(DISTINCT kind, ',' ORDER BY kind) FROM sift.table_segments('w')
