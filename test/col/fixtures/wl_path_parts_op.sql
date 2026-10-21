-- the @? operator, && and || of members and the path as an aggregate FILTER
-- shape: S6
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.mix,
	   count(*) FILTER (WHERE coldiff.doc_norm(w.doc) @? '$.parts[*] ? (@.dir_a == "down" && @.category == "catg_j")') AS neg_catg_j,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.category == "catg_b" || @.category == "catg_e")')) AS place,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.val == "catg_n")')) AS catg_n,
	   min(w.id) AS first_id
FROM t_work w
GROUP BY 1
ORDER BY 1 NULLS FIRST
