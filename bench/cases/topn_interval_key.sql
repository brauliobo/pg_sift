-- ORDER BY an interval column LIMIT n: the top-N reads the key through the dictionary of its datums
-- label: `ORDER BY lead LIMIT 10` over an `interval` column
-- ordered: yes
--@ setup
ALTER TABLE orders ADD COLUMN lead interval;
UPDATE orders SET lead = ((id::bigint * 7919) % 1000003) * interval '1 second' + (id % 7) * interval '1 day';
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col ({cols}, lead)
--@ pg
SELECT lead FROM orders ORDER BY lead LIMIT 10
