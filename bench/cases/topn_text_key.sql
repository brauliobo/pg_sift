-- ORDER BY a text column LIMIT n: the entries of the unit's dictionary are the keys
-- label: `ORDER BY title LIMIT 10`
-- ordered: yes
--@ pg
SELECT title FROM orders ORDER BY title LIMIT 10
