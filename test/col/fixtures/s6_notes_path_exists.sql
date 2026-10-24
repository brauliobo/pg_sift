-- S6: orders per country and language with a note of kind kind_a by role_a among the notes (jsonb_path_exists on an array of objects), and their sum
-- shape: S6
-- expect: agg
-- sets: K400 K4000
-- key: -
-- scenario: yes
SELECT country_code, lang, count(*) AS orders, count(*) FILTER (WHERE answered) AS answered, sum(store_id) FILTER (WHERE answered) AS answered_stores
FROM (SELECT o.country_code, o.lang, o.store_id, jsonb_path_exists(o.notes, '$[*] ? (@.type == "kind_a" && @.author_role == "role_a")') AS answered
      FROM orders o WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL AND {range(o.placed_at)}) x
GROUP BY 1, 2
ORDER BY 1 NULLS FIRST, 2
