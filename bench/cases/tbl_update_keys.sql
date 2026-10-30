-- 40,000 UPDATEs by primary key of 2,000 rows in the column groups of a sift_table of 600,000 rows, 20 statements over the same rows, their secondary index keys unchanged,
-- with 100,000 rows in the write buffer: the updates are HOT (the new versions have no index entry; the entries of the old ones lead to them through the tombstone log), as the heap's
-- label: `UPDATE t SET amount = amount + 1 WHERE id IN (<2,000 ids>)`, 20 statements over the same rows, a `sift_table` of 600,000 orders against a heap table, both with the primary key and the btrees of the store
--@ prepare
CREATE TABLE upd (id bigint, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING {am};
INSERT INTO upd SELECT id, customer_id, country, category, status, amount, quantity, created_at, title FROM public.orders_src WHERE id <= 600000 ORDER BY id;
VACUUM upd;
ALTER TABLE upd ADD PRIMARY KEY (id);
CREATE INDEX upd_created ON upd (created_at);
CREATE INDEX upd_country_status ON upd (country, status);
INSERT INTO upd SELECT id + 100000000, customer_id, country, category, status, amount, quantity, created_at, title FROM upd WHERE id <= 100000;
ANALYZE upd
--@ load
DO $$ BEGIN FOR r IN 0..19 LOOP UPDATE upd SET amount = amount + 1 WHERE id IN (SELECT g * 300 + 7 FROM generate_series(1, 2000) g); END LOOP; END $$
--@ check
SELECT string_agg(DISTINCT kind, ',' ORDER BY kind) FROM sift.table_segments('upd')
