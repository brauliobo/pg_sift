-- S7: existential flags in FILTER clauses of one query: EXISTS over recordset columns and over jsonb_array_elements, with the plain count beside
-- shape: S7
-- expect: agg
-- sets: K4000
-- key: -
SELECT o.country_code, count(*) AS orders,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(qty int, price float8) WHERE i.qty >= 3 AND i.price < 20)) AS cheap_bulk,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_array_elements(o.notes) n WHERE n->>'type' = 'internal' AND n->>'author_role' = 'role_b')) AS internal_by_customer
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
GROUP BY 1
ORDER BY 1 NULLS FIRST
