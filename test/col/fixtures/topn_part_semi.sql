-- a sub-select over a partitioned table's Top-N
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp, d FROM t_part WHERE grp IN (SELECT id FROM tenants WHERE kind IN ('outlet', 'popup')) ORDER BY d DESC, id LIMIT 15
