-- one group per row: the 10 largest orders by value, as a GROUP BY of the primary key (1,000,000 groups at 1,000,000 orders)
-- label: `GROUP BY id ORDER BY sum(amount * quantity) DESC LIMIT 10`, one group per order
-- ordered: yes
--@ pg
SELECT id, sum(amount * quantity) AS value FROM orders GROUP BY id ORDER BY value DESC, id LIMIT 10
