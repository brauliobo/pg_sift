-- WHERE uuid = constant and uuid IN (...) over distinct values: a test of the source on the stored image, not the operator on a dictionary of datums
-- label: `count(*) WHERE ref IN (C1, C2, C3)` over a `uuid` column of distinct values
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN ref uuid;
UPDATE orders SET ref = md5(id::text)::uuid;
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, ref)
--@ pg
SELECT count(*) AS n FROM orders WHERE ref IN (md5('123457')::uuid, md5('7')::uuid, md5('20001')::uuid)
