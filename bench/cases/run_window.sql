-- a thirty-day range of a timestamp with microseconds that arrives in no order (no zone cuts a unit): the sorted run of each unit finds the rows of the range by binary search, where a unit without one tests every row of a 64-bit-wide chunk
-- label: `count(*), sum(amount) WHERE created_at` in a thirty-day range of a microsecond timestamp, `order_columns = 'created_at'`; the variant `no_run` is the same index without the run
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS run_window_plain CASCADE;
CREATE SCHEMA run_window_plain;
CREATE TABLE orders_us AS SELECT id, amount, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_us_created ON orders_us (created_at);
CREATE INDEX orders_us_col ON orders_us USING sift_col (amount, created_at) WITH (order_columns = 'created_at');
CREATE TABLE run_window_plain.orders_us AS TABLE orders_us;
CREATE INDEX orders_us_col ON run_window_plain.orders_us USING sift_col (amount, created_at);
VACUUM (FREEZE, ANALYZE) orders_us;
VACUUM (FREEZE, ANALYZE) run_window_plain.orders_us
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders_us WHERE created_at >= timestamp '2024-06-10 13:00' AND created_at < timestamp '2024-07-10 13:00'
