-- a stable key with a joined table's column
-- shape: group
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, (current_setting('server_version_num')::int > 0) AS k, count(*) AS n FROM orders o JOIN stores s ON s.id = o.store_id GROUP BY 1, 2 ORDER BY 1
