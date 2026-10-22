-- GROUP BY the month of a timestamp, from the calendar cells of the units
-- label: `sum(amount), count(*) GROUP BY date_trunc('month', created_at)`
-- ordered: yes
--@ pg
SELECT date_trunc('month', created_at)::date AS month, sum(amount), count(*) FROM orders GROUP BY 1 ORDER BY 1
