-- a range no partition holds: only the default partition
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, min(d) AS first_day, max(d) AS last_day FROM t_part WHERE d < DATE '2024-01-01' GROUP BY grp ORDER BY grp
