-- n + m past 100,000: the top-N scan keeps every row that passes and sorts them
-- label: `ORDER BY amount LIMIT 20 OFFSET 150000`
-- ordered: yes
--@ pg
SELECT amount FROM orders ORDER BY amount LIMIT 20 OFFSET 150000
