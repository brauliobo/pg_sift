-- a stable key beside a column key
-- shape: group
-- expect: agg
-- key: -
-- scenario: yes
SELECT (current_setting('server_version_num')::int > 0) AS k, status, count(*) AS n FROM orders GROUP BY 1, 2 ORDER BY 2
