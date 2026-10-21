-- EXISTS over the items that compares an item with a column of the joined store (no lookup), beside another condition of the item
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.country_code, count(*) AS n, avg(o.total) AS total
FROM orders o JOIN stores s ON s.id = o.store_id
WHERE o.store_id < 300 AND o.deleted_at IS NULL AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
    WHERE i.qty IS NOT NULL AND i.price > 10 AND (i.kind = s.kind OR (i.category = 'Home' AND s.country_code = 'US')))
GROUP BY s.country_code ORDER BY s.country_code NULLS FIRST
