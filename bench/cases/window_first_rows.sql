-- the first 3 rows of each group by a window function, over one month: the group's arrays of each column, ordered and cut at 3, taken when the planner costs the grouped scan below the window
-- label: `WHERE rn <= 3` over `row_number() OVER (PARTITION BY segment_id ORDER BY created_at DESC, id)`, one month of the orders, 6,313 groups
-- ordered: yes
--@ pg
SELECT segment_id, id, created_at, rn FROM (SELECT segment_id, id, created_at, row_number() OVER (PARTITION BY segment_id ORDER BY created_at DESC, id) AS rn FROM orders
WHERE created_at >= timestamp '2025-03-01' AND created_at < timestamp '2025-04-01') s WHERE rn <= 3 ORDER BY segment_id, rn
