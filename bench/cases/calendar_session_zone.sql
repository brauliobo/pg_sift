-- GROUP BY the day of a timestamptz in the session's time zone (daylight saving time inside the data): the scan numbers the days of the zone's clock and evaluates date_trunc once per day
-- label: `sum(amount), count(*) GROUP BY date_trunc('day', placed_at)`, `placed_at` a `timestamptz` read in `America/New_York`
-- ordered: yes
-- set: TimeZone = 'America/New_York'
--@ setup
ALTER TABLE orders ADD COLUMN placed_at timestamptz;
UPDATE orders SET placed_at = created_at AT TIME ZONE 'UTC';
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id sift.col_int4_ops, placed_at sift.col_timestamptz_ops, amount sift.col_numeric_ops)
--@ pg
SELECT date_trunc('day', placed_at) AS day, sum(amount), count(*) FROM orders GROUP BY 1 ORDER BY 1
