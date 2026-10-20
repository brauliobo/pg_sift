-- the group by is a function of the partition key
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT date_trunc('month', d)::date AS month, count(*) AS n, sum(qty) AS q FROM t_part GROUP BY 1 ORDER BY 1
