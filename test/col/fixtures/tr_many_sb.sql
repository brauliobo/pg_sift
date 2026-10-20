-- ctid: a range from the middle of the first superblock to the end of the table
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT lang, count(*) AS n, sum(total) AS t FROM orders WHERE ctid >= '(100,1)' GROUP BY lang ORDER BY lang NULLS FIRST
