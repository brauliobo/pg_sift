-- ORDER BY in the call over no rows, and over groups whose inputs are all NULL (array_agg keeps them, string_agg skips them)
-- shape: flat
-- expect: agg
-- key: -
SELECT source, array_agg(country_code ORDER BY id) FILTER (WHERE country_code IS NULL) AS nulls, string_agg(country_code, ',' ORDER BY id) FILTER (WHERE country_code IS NULL) AS nulls_s,
       array_agg(id ORDER BY id) FILTER (WHERE id < 0) AS none, count(*) AS n
FROM orders WHERE store_id < 40 GROUP BY source ORDER BY source
