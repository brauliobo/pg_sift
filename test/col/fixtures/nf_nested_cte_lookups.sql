-- a ranked aggregate whose derived table has its own WITH: the CTE is a lookup of the scan, the derived table reads nothing around it
-- shape: S5
-- expect: any
-- key: -
-- scenario: yes
SELECT r.kind, r.n, r.rn FROM (
  SELECT t.*, row_number() OVER (ORDER BY t.n DESC, t.kind) AS rn FROM (
    WITH scope AS MATERIALIZED (SELECT DISTINCT id FROM stores WHERE country_code <> 'ZZ')
    SELECT s.kind, count(*) AS n FROM orders o JOIN scope c ON c.id = o.store_id JOIN stores s ON s.id = c.id WHERE o.deleted_at IS NULL GROUP BY s.kind
  ) t
) r WHERE r.rn <= 3
ORDER BY r.rn
