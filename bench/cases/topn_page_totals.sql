-- A page of rows with the total of its set
-- label: `.., count(*) OVER (), sum(amount) OVER () WHERE country = 'US' ORDER BY created_at DESC LIMIT 10 OFFSET 20`
-- ordered: yes
--@ pg
SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue
FROM orders WHERE country = 'US' ORDER BY created_at DESC LIMIT 10 OFFSET 20
