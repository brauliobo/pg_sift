-- ctid: a range of orders, grouped by an element of their items
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT i.category, count(*) AS n, sum(i.qty) AS q FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int) WHERE o.ctid >= '(30,1)' AND o.ctid < '(310,1)' GROUP BY i.category ORDER BY i.category NULLS FIRST
