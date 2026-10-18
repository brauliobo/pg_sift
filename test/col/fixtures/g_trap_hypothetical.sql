-- trap: a hypothetical-set aggregate keeps the normal plan
-- shape: trap
-- expect: plain
-- key: -
SELECT source, rank(10) WITHIN GROUP (ORDER BY tenant_id) AS r, dense_rank(10) WITHIN GROUP (ORDER BY tenant_id) AS dr FROM orders GROUP BY source ORDER BY source
