-- generic aggregates over columns of a lookup-joined table: the value of each joined row (NULL for a LEFT JOIN row without a match) goes through PostgreSQL's transition function
-- shape: S1
-- expect: agg
-- key: -
SELECT o.source, array_agg(s.kind ORDER BY o.id) AS kinds, string_agg(DISTINCT s.country_code, ',') AS ccs, min(s.name) AS lo, max(upper(s.kind)) AS hi,
       string_agg(DISTINCT s.kind, ',' ORDER BY s.kind DESC) AS sorted_kinds, mode() WITHIN GROUP (ORDER BY s.country_code) AS mode_cc
FROM orders o LEFT JOIN tenants s ON s.id = o.tenant_id
WHERE o.tenant_id < 600 GROUP BY o.source ORDER BY o.source
