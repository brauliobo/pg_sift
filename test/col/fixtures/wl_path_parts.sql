-- jsonb_path_exists on the nested array of a document that is an index expression: a lax $.parts[*] with a string member of an astext leaf
-- shape: S6
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.dim_a_id % 5 AS g, count(*) AS n, sum(w.id) AS ids
FROM t_work w
WHERE w.deleted_at IS NULL AND jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.dir_a == "up")')
GROUP BY 1
ORDER BY 1
