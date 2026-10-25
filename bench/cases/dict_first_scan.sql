-- the first statement of a backend over a text column with a large dictionary, 82 rows: the dictionary is read from the segment with the default `dict_stored` (the percent of a segment that a stored, uncompressed dictionary may add)
-- label: `count(*)`, `sum(amount)` grouped by a text column with a large dictionary over 82 rows, the first statement of a new backend
-- ordered: yes
-- fresh: yes
--@ pg
SELECT title, count(*) AS n, sum(amount) AS revenue FROM orders WHERE id BETWEEN 5000 AND 5081 GROUP BY 1 ORDER BY 1
