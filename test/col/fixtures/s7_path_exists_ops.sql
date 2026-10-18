-- S7: jsonb_path_exists and @? with integer and boolean comparisons, || and a literal on the left; NOT of the test on a NOT NULL column
-- shape: S7
-- expect: agg
-- sets: K4000
-- key: -
SELECT o.lang, count(*) AS orders,
       count(*) FILTER (WHERE jsonb_path_exists(o.items, '$[*] ? (@.qty >= 4 && @.category == "Toys")')) AS bulk_toys,
       count(*) FILTER (WHERE o.items @? '$[*] ? (@.qty < 2 || @.sku == "SKU-7")') AS small_or_sku,
       count(*) FILTER (WHERE jsonb_path_exists(o.notes, '$[*] ? ("comment" == @.type)')) AS comment,
       count(*) FILTER (WHERE NOT jsonb_path_exists(o.notes, '$[*] ? (@.type == "internal")')) AS no_internal
FROM orders o
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
GROUP BY 1
ORDER BY 1
