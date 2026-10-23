-- count(DISTINCT) of a column of many values over groups that hold a few of them each: the sets start as hash sets, not as bitmaps of a directory and a chunk for every value
-- label: `count(DISTINCT customer_id)` by `segment_id` (6,313 groups of about 30 rows)
-- ordered: yes
--@ pg
SELECT segment_id, count(DISTINCT customer_id) FROM orders GROUP BY 1 ORDER BY 1 LIMIT 1000
