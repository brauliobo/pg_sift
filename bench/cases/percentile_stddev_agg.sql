-- an ordered-set aggregate beside a statistical one: the 95th percentile and the standard deviation of the amount of the shipped orders of each country
-- label: `percentile_cont(0.95) WITHIN GROUP (ORDER BY amount)`, `stddev(amount)` of the shipped orders by country
-- ordered: yes
--@ pg
SELECT country, count(*), percentile_cont(0.95) WITHIN GROUP (ORDER BY amount) AS p95, stddev(amount) AS sd FROM orders WHERE status = 'shipped' GROUP BY 1 ORDER BY 1
