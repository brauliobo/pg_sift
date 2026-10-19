-- cost: a selective set of orders summing a numeric column keeps the normal plan (the scan reads every unit, the index lookup 40 rows)
-- shape: S6
-- expect: plain
-- key: -
SELECT country_code, lang, count(*) AS orders, sum(total) AS total
FROM orders WHERE id < 40 AND deleted_at IS NULL
GROUP BY 1, 2 ORDER BY 1, 2
