-- the index scan of a sift_col index over a table that is not all-visible (never vacuumed, or a standby): the rows it selects are checked against the heap decoding the offsets of the pages that hold a selected row only
-- label: `SELECT count(*), sum(amount) WHERE customer_id = <one customer> AND created_at in a month` over `orders` never vacuumed (empty visibility map); the variant `vacuumed` is the same index over a copy after `VACUUM (FREEZE)`
-- ordered: no
-- vacuum: no
--@ setup
ANALYZE orders;
DROP SCHEMA IF EXISTS col_scan_not_all_visible_vacuumed CASCADE;
CREATE SCHEMA col_scan_not_all_visible_vacuumed;
CREATE TABLE col_scan_not_all_visible_vacuumed.orders AS TABLE orders;
CREATE INDEX orders_col ON col_scan_not_all_visible_vacuumed.orders USING sift_col ({cols});
VACUUM (FREEZE, ANALYZE) col_scan_not_all_visible_vacuumed.orders
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id = 137 AND created_at >= '2024-03-01' AND created_at < '2024-04-01'
