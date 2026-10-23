-- ORDER BY an enum column LIMIT n: the scan ranks the OIDs of the values by the order of the type
-- label: `ORDER BY lvl LIMIT 10` over an `enum` column
-- ordered: yes
--@ setup
DROP TYPE IF EXISTS ord_lvl CASCADE;
CREATE TYPE ord_lvl AS ENUM ('warn', 'info', 'error');
ALTER TYPE ord_lvl ADD VALUE 'debug' BEFORE 'info';
ALTER TABLE orders ADD COLUMN lvl ord_lvl;
UPDATE orders SET lvl = (enum_range(NULL::ord_lvl))[1 + id % 4];
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, lvl)
--@ pg
SELECT lvl FROM orders ORDER BY lvl LIMIT 10
