-- trap: a WHERE the NULL row of the order table passes keeps the outer join (the anti join of the stores without orders): the normal plan
-- shape: trap
-- expect: plain
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS tenants
FROM tenants s LEFT JOIN orders o ON o.tenant_id = s.id
WHERE o.id IS NULL
GROUP BY s.kind ORDER BY s.kind
