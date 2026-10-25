-- the same statement with `dict_stored = 17`: a dictionary is stored uncompressed while that adds up to 17% to the segment, and the first scan of a backend copies it where it would inflate it
-- label: `count(*)`, `sum(amount)` grouped by a text column with a large dictionary over 82 rows, the first statement of a new backend, `dict_stored = 17`
-- ordered: yes
-- fresh: yes
--@ pg
SELECT title, count(*) AS n, sum(amount) AS revenue FROM orders WHERE id BETWEEN 5000 AND 5081 GROUP BY 1 ORDER BY 1
