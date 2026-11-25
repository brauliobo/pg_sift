-- a group key that reads no column (a stable function) is one group, evaluated once
-- shape: group
-- expect: agg
-- key: -
-- scenario: yes
SELECT (current_setting('server_version_num')::int > 0) AS k, count(*) AS n, sum(total) AS revenue FROM orders GROUP BY 1
