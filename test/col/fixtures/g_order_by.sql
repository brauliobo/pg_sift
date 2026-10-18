-- generic aggregates with ORDER BY in the call: a tuplesort per group on the sort keys (direction, NULLS FIRST/LAST, a key that is not an argument), then the transition function
-- shape: flat
-- expect: agg
-- key: -
SELECT source, string_agg(country_code, ',' ORDER BY country_code DESC NULLS FIRST, id) AS ccs, array_agg(id ORDER BY store_id DESC, placed_at, id) AS ids,
       jsonb_agg(status ORDER BY id DESC) AS st, json_agg(placed_at ORDER BY placed_at DESC NULLS LAST, id) AS days,
       array_agg(total ORDER BY total NULLS FIRST, id) FILTER (WHERE store_id % 7 = 0) AS tot,
       string_agg(lang, '|' ORDER BY lang COLLATE "C", id) AS langs, array_agg(DISTINCT store_id) AS stores
FROM orders WHERE store_id < 30 GROUP BY source ORDER BY source
