-- the groups of an aggregate under a window on their keys, in the window's order from the scan: no Sort above it
-- label: `lag(n) OVER (PARTITION BY category ORDER BY segment_id DESC)` over `count(*) GROUP BY category, segment_id`, 6,313 segments
-- ordered: yes
--@ pg
SELECT category, segment_id, n, lag(n) OVER (PARTITION BY category ORDER BY segment_id DESC) AS previous FROM (SELECT category, segment_id, count(*) AS n FROM orders GROUP BY category, segment_id) g ORDER BY category, segment_id DESC
