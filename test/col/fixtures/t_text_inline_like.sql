-- flat: a prefix of long text, grouped by the long value
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT t, count(*) AS n FROM t_text WHERE t LIKE repeat('b', 260) || '%' GROUP BY t ORDER BY t
