-- the same as a conjunct of the WHERE, with a date and a key of the table, and the array of a text column
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.item_date >= date '2024-01-01' AS recent, w.country_code, count(*) AS n, sum(w.id) AS ids
FROM t_load w
WHERE w.deleted_at IS NULL AND w.entity_id = ANY (ARRAY(SELECT e.id FROM t_load_entity e WHERE e.type IN ('cat_b', 'cat_c')))
  AND w.country_code = ANY (ARRAY(SELECT x.country_code FROM t_load x WHERE x.id <= 30))
GROUP BY 1, 2
ORDER BY 1, 2 NULLS FIRST
