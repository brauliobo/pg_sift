-- eight group columns, the limit: text, varchar, date, boolean and integer keys, NULLs in every position, a FILTER and a distinct count
-- shape: flat
-- expect: agg
-- sets: K4000 K400
-- key: -
-- scenario: yes
SELECT o.source, o.status, o.lang, o.country_code, date_trunc('year', o.placed_at)::date AS yr, o.store_id % 3 AS st3,
       o.deleted_at IS NULL AS live, o.customer_id IS NULL AS anon,
       count(*) AS n, sum(o.total) AS revenue, count(*) FILTER (WHERE o.discount > 20) AS discounted, count(DISTINCT o.store_id) AS stores
FROM orders o
WHERE o.store_id IN ({stores})
GROUP BY 1, 2, 3, 4, 5, 6, 7, 8
ORDER BY 1, 2 NULLS FIRST, 3, 4 NULLS FIRST, 5, 6, 7, 8
