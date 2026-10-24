-- mode() of a group is the most frequent entry of the histogram of its values, where it went through the generic aggregate
-- label: `mode()` of `amount` and of `quantity` by `customer_id % 40` (about 25,000 values a group)
-- ordered: yes
--@ pg
SELECT customer_id % 40 AS grp, mode() WITHIN GROUP (ORDER BY amount) AS common_amount, mode() WITHIN GROUP (ORDER BY quantity) AS common_quantity FROM orders GROUP BY 1 ORDER BY 1
