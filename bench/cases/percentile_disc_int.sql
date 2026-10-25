-- percentile_disc of a decimal, a timestamp and a bigint is found on the 64-bit values of the histogram, where it went through the generic aggregate
-- label: `percentile_disc(0.5)` of `amount` and of `created_at` by `customer_id % 40` (about 25,000 values a group)
-- ordered: yes
--@ pg
SELECT customer_id % 40 AS grp, percentile_disc(0.5) WITHIN GROUP (ORDER BY amount) AS median_amount, percentile_disc(0.5) WITHIN GROUP (ORDER BY created_at) AS median_at FROM orders GROUP BY 1 ORDER BY 1
