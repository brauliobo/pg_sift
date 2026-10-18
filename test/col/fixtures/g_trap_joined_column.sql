-- trap: a generic aggregate of a column of a joined table keeps the normal plan (only the mapped aggregates read joined columns)
-- shape: trap
-- expect: plain
-- key: -
SELECT source, (SELECT string_agg(v, ',' ORDER BY v) FROM unnest(string_to_array(k, ',')) v) AS kinds
FROM (SELECT o.source, string_agg(s.kind, ',') AS k FROM orders o JOIN tenants s ON s.id = o.tenant_id GROUP BY o.source) q
ORDER BY source
