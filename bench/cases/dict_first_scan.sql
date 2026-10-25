-- the first statement of a backend over a text column with a large dictionary, 82 rows: the strings of the groups are read from the segment, not the whole dictionary
-- label: `count(*)`, `sum(amount)` grouped by a text column with a large dictionary over 82 rows, the first statement of a new backend
-- ordered: yes
-- fresh: yes
--@ pg
SELECT title, count(*) AS n, sum(amount) AS revenue FROM orders WHERE id BETWEEN 5000 AND 5081 GROUP BY 1 ORDER BY 1
