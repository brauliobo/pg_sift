-- ORDER BY lower(text column) LIMIT n
-- label: `ORDER BY lower(category) LIMIT 10`
-- ordered: yes
--@ pg
SELECT category FROM orders ORDER BY lower(category) LIMIT 10
