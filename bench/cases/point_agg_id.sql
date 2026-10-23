-- a lookup by the primary key through the aggregate scan, against the btree of the key
-- label: `sum(amount) WHERE id = <one id>`, the primary key btree against `point_tid_columns = 'id'`
--@ pg
SELECT sum(amount) AS revenue, count(*) AS n FROM orders WHERE id = 424242
