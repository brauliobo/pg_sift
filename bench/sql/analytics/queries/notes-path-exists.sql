-- notes-path-exists: orders per country and language with a note of kind kind_a by type_a among their notes (a jsonb_path_exists flag on an array of objects), and their total
-- sets: K10 K400 K4000
-- order: 6
-- key: 1 2
--@ pg
SELECT country_code, lang, count(*) AS orders, count(*) FILTER (WHERE answered) AS answered, sum(total) FILTER (WHERE answered) AS answered_total
FROM (SELECT o.country_code, o.lang, o.total,
             jsonb_path_exists(o.notes, '$[*] ? (@.type == "kind_a" && @.author_kind == "type_a")') AS answered
      FROM orders o
      WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}) x
GROUP BY 1, 2
--@ flat
SELECT country_code, lang, count(*) AS orders, count(*) FILTER (WHERE answered) AS answered, sum(total) FILTER (WHERE answered) AS answered_total
FROM (SELECT o.country_code, o.lang, o.total,
             EXISTS (SELECT 1 FROM pg_notes n WHERE n.order_id = o.id AND n.type = 'kind_a' AND n.author_kind = 'type_a') AS answered
      FROM orders o
      WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}) x
GROUP BY 1, 2
