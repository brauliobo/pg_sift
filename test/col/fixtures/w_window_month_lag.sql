-- window: month over month change of a numeric sum, with a filter and a HAVING under the window
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT date_trunc('month', placed_at) AS month, sum(total) AS revenue, lag(sum(total)) OVER (ORDER BY date_trunc('month', placed_at)) AS before,
       sum(sum(total)) OVER (ORDER BY date_trunc('month', placed_at)) AS running
FROM orders
WHERE deleted_at IS NULL AND status IS DISTINCT FROM 'cancelled'
GROUP BY date_trunc('month', placed_at)
HAVING count(*) > 2
ORDER BY 1
