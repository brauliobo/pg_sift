-- a band of scores, written as BETWEEN, ordered by another column
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT id, tenant_id FROM orders
WHERE memo @@> 'fast OR late OR gift' AND sift.score(ctid) BETWEEN 1.5 AND 3 AND deleted_at IS NULL
ORDER BY tenant_id, id LIMIT 60
