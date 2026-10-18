-- S6: orders per country and language with a staff reply among the notes (jsonb_path_exists on an array of objects)
-- shape: S6
-- expect: any
-- sets: K400 K4000
-- key: -
SELECT country_code, lang, count(*) AS orders, count(*) FILTER (WHERE answered) AS answered
FROM (SELECT o.country_code, o.lang, jsonb_path_exists(o.notes, '$[*] ? (@.type == "reply" && @.author_role == "staff")') AS answered
      FROM orders o WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL AND {range(o.placed_at)}) x
GROUP BY 1, 2
ORDER BY 1 NULLS FIRST, 2
