-- preagg: a list of the key's values: the groups of the histogram that pass
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, sum(total) AS revenue
FROM orders
WHERE status IN ('paid', 'shipped', 'nothing')
GROUP BY status
ORDER BY status
