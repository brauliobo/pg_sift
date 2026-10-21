-- the @? operator, && and || of members and the path as an aggregate FILTER
-- shape: S6
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.mix,
	   count(*) FILTER (WHERE coldiff.load_norm(w.doc) @? '$.parts[*] ? (@.sign == "negative" && @.category == "asp_j")') AS neg_asp_j,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.load_norm(w.doc), '$.parts[*] ? (@.category == "asp_b" || @.category == "asp_e")')) AS place,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.load_norm(w.doc), '$.parts[*] ? (@.val == "asp_n")')) AS asp_n,
	   min(w.id) AS first_id
FROM t_load w
GROUP BY 1
ORDER BY 1 NULLS FIRST
