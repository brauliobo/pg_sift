-- no rows, no group (a global aggregate would give one row)
-- shape: group
-- expect: agg
-- key: -
-- scenario: yes
SELECT (current_setting('server_version_num')::int > 0) AS k, count(*) AS n FROM orders WHERE total < 0 GROUP BY 1
