-- the last range, the unlogged partition and the default one
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT tag, count(*) AS n, sum(qty) AS q FROM t_part WHERE d >= DATE '2024-10-01' GROUP BY tag ORDER BY tag
