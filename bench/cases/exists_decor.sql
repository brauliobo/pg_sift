-- EXISTS in the select list and NOT EXISTS under an OR, where PostgreSQL runs a sub-plan for each outer row: the table grouped by the tie column, left joined to the outer rows
-- label: `SELECT id, EXISTS (SELECT 1 FROM orders f WHERE f.segment_id = o.id AND f.amount > 290)`, and `id < 100 OR NOT EXISTS (..)`, 2,000 outer rows, a btree on the tie column
-- ordered: yes
--@ setup
CREATE TABLE key_sets AS SELECT g AS id FROM generate_series(1, 2000) g;
ANALYZE key_sets;
CREATE INDEX orders_seg_btree ON orders (segment_id)
--@ pg
SELECT o.id, EXISTS (SELECT 1 FROM orders f WHERE f.segment_id = o.id AND f.amount > 290) AS big,
       (o.id < 100 OR NOT EXISTS (SELECT 1 FROM orders f WHERE f.segment_id = o.id AND f.status = 'cancelled' AND f.amount > 290)) AS keep
FROM key_sets o ORDER BY o.id
