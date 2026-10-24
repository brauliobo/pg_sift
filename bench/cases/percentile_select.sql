-- a percentile of a group is found by a quickselect of its positions over the histogram of the group's values, where the histogram was sorted whole
-- label: `percentile_cont(0.5)` and `percentile_cont(0.95)` of `amount::float8` by `customer_id % 40` (about 100,000 values a group)
-- ordered: yes
--@ pg
SELECT customer_id % 40 AS grp, percentile_cont(0.5) WITHIN GROUP (ORDER BY amount::float8) AS median, percentile_cont(0.95) WITHIN GROUP (ORDER BY amount::float8) AS p95 FROM orders GROUP BY 1 ORDER BY 1
