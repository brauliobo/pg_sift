-- a thirty-day range of a timestamp with microseconds that arrives in no order: its chunk holds 6-byte plain values, which the range test reads with one 8-byte load each
-- label: `count(*), sum(amount) WHERE created_at` in a thirty-day range of a microsecond timestamp, a sift_col index without sorted runs
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS orders_us;
CREATE TABLE orders_us AS SELECT id, amount, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_us_created ON orders_us (created_at);
CREATE INDEX orders_us_col ON orders_us USING sift_col (amount, created_at);
VACUUM (FREEZE, ANALYZE) orders_us
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders_us WHERE created_at >= timestamp '2024-06-10 13:00' AND created_at < timestamp '2024-07-10 13:00'
