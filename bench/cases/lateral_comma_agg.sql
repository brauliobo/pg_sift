-- the comma form of a lateral aggregate: an inner join of the sub-select to the item of the FROM list, decorrelated like the explicit join
-- label: `FROM owners o, LATERAL (SELECT count(*), sum(amount) FROM orders f WHERE f.segment_id = o.id)`, 2,000 outer rows, a btree on the tie column
-- ordered: yes
--@ setup
CREATE TABLE key_sets AS SELECT g AS id FROM generate_series(1, 2000) g;
ANALYZE key_sets;
CREATE INDEX orders_seg_btree ON orders (segment_id)
--@ pg
SELECT o.id % 10 AS bucket, sum(l.c) AS orders, sum(l.s) AS total
FROM key_sets o, LATERAL (SELECT count(*) AS c, sum(f.amount) AS s FROM orders f WHERE f.segment_id = o.id) l
GROUP BY 1 ORDER BY 1
