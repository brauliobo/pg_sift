-- FULL JOIN on a text key: a NULL key on both sides matches nothing, a status without a row and a row without a status are unmatched
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT d.stage, o.status, count(*) AS rows, count(o.id) AS orders, count(d.status) AS rows_of_dim
FROM orders o FULL JOIN dim_status d ON d.status = o.status
GROUP BY d.stage, o.status ORDER BY d.stage NULLS FIRST, o.status NULLS FIRST
