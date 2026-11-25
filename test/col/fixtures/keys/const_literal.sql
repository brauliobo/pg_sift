-- a constant as the only group key: one group when the scan has rows
-- shape: group
-- expect: agg
-- key: -
-- scenario: yes
SELECT 'x' AS k, count(*) AS n, max(total) AS top FROM orders GROUP BY 1
