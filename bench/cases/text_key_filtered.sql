-- a near-unique text key under a filter that keeps few groups: the ids of every dictionary entry are not worth their table
-- label: `GROUP BY title` with `WHERE country = 'US' AND amount > 900`, `count`, `sum`
-- ordered: yes
--@ pg
SELECT title, count(*), sum(amount) FROM orders WHERE country = 'US' AND amount > 900 GROUP BY title ORDER BY 2 DESC, 1 LIMIT 20
