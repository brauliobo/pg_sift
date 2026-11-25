-- HAVING and FILTER over a partitioned table
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT tag, count(*) FILTER (WHERE qty > 10) AS busy, sum(amount) AS revenue FROM t_part GROUP BY tag HAVING count(*) > 100 ORDER BY tag
