-- the same as a conjunct of the WHERE, with a date and a key of the table, and the array of a text column
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.item_date >= date '2024-01-01' AS recent, w.country_code, count(*) AS n, sum(w.id) AS ids
FROM t_work w
WHERE w.deleted_at IS NULL AND w.dim_a_id = ANY (ARRAY(SELECT e.id FROM t_work_dim e WHERE e.type IN ('kind_b', 'venue')))
  AND w.country_code = ANY (ARRAY(SELECT x.country_code FROM t_work x WHERE x.id <= 30))
GROUP BY 1, 2
ORDER BY 1, 2 NULLS FIRST
