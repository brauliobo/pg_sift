-- paths of a jsonb column as group key and measure
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT doc ->> 's' AS s, count(*) AS n, sum((doc ->> 'k')::int) AS k FROM t_part GROUP BY 1 ORDER BY 1
