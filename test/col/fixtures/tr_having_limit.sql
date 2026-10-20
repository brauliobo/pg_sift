-- ctid: HAVING and LIMIT over a range
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT store_id, count(*) AS n FROM orders WHERE ctid >= '(30,1)' AND ctid < '(520,1)' GROUP BY store_id HAVING count(*) > 5 ORDER BY n DESC, store_id LIMIT 10
