-- count(DISTINCT) of a 32-bit column from the histograms of the cube
-- label: `count(DISTINCT customer_id)` (40,000 values)
--@ pg
SELECT count(DISTINCT customer_id) FROM orders
