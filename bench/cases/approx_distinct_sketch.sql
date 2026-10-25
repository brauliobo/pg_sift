-- sift.approx_count_distinct of a whole table from the sketches its units store, against the same aggregate by the normal plan
-- label: `sift.approx_count_distinct(title)` and `(customer_id)` (`hll_columns`)
--@ pg
SELECT sift.approx_count_distinct(title) AS titles, sift.approx_count_distinct(customer_id) AS customers FROM orders
--@ duck
SELECT approx_count_distinct(title) AS titles, approx_count_distinct(customer_id) AS customers FROM orders
