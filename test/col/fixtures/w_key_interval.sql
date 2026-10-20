-- a key of interval: 1 month, 30 days and 720 hours are one group; only the groups are counted
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT count(*) AS groups, sum(n) AS rows, max(n) AS biggest FROM (SELECT iv, count(*) AS n FROM t_wide GROUP BY iv) g
