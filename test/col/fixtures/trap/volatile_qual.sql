-- trap: a volatile function in the qual (random() < 2 is always true, but the planner cannot know)
-- shape: trap
-- expect: plain
-- key: -
SELECT source, count(*) AS n, sum(tenant_id) AS s FROM orders WHERE random() < 2 AND tenant_id < 200 GROUP BY source ORDER BY source
