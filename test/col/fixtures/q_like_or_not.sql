-- quals: LIKE on a dictionary column, OR across columns, NOT and CASE
-- shape: quals
-- expect: any
-- key: -
SELECT count(*) AS n, count(DISTINCT source) AS sources
FROM orders
WHERE (source LIKE 'marketplace%' OR tenant_id < 20 OR country_code = 'DE')
  AND NOT (status = 'cancelled')
  AND CASE WHEN tenant_id < 100 THEN lang IN ('en', 'de') ELSE source <> 'web' END
