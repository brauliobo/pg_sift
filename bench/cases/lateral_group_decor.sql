-- a lateral sub-select that groups, for each of 2,000 outer rows: the table is grouped by the tie column and the group once, not looked up for each outer row
-- label: `x JOIN LATERAL (SELECT status, count(*), sum(amount) FROM orders f WHERE f.segment_id = x.id GROUP BY status) ON true`, 2,000 outer rows, a btree on the tie column
-- ordered: yes
--@ setup
CREATE TABLE key_sets AS SELECT g AS id FROM generate_series(1, 2000) g;
ANALYZE key_sets;
CREATE INDEX orders_seg_btree ON orders (segment_id)
--@ pg
SELECT l.status, count(*) AS n, sum(l.c) AS orders, sum(l.s) AS total
FROM key_sets o JOIN LATERAL (SELECT f.status, count(*) AS c, sum(f.amount) AS s FROM orders f WHERE f.segment_id = o.id GROUP BY f.status) l ON true
GROUP BY l.status ORDER BY l.status
