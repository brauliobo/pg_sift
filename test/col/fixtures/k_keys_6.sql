-- six group columns of mixed widths (dictionary text, a varchar, a date bucket, an integer expression) with NULL keys, a FILTER and an expression key
-- shape: flat
-- expect: agg
-- sets: K4000 K400
-- key: -
-- scenario: yes
SELECT o.source, o.status, o.lang, o.country_code, date_trunc('year', o.placed_at)::date AS yr, o.store_id % 3 AS st3,
       count(*) AS n, sum(o.total) AS revenue, count(*) FILTER (WHERE o.discount > 20) AS discounted, min(o.placed_at) AS first_day
FROM orders o
WHERE o.store_id IN ({stores})
GROUP BY 1, 2, 3, 4, 5, 6
ORDER BY 1, 2 NULLS FIRST, 3, 4 NULLS FIRST, 5, 6
