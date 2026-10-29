-- A page of rows with the total of its set
-- label: `.., count(*) OVER (), sum(amount) OVER () WHERE country = 'US' ORDER BY created_at DESC LIMIT 10 OFFSET 20`
-- ordered: ties 3
--@ pg
SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue
FROM orders WHERE country = 'US' ORDER BY created_at DESC LIMIT 10 OFFSET 20
--@ ties
SELECT * FROM (SELECT id, amount, created_at, count(*) OVER () AS total, sum(amount) OVER () AS revenue FROM orders WHERE country = 'US') o WHERE created_at = '{3}'
