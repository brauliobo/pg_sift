-- the @? operator, && and || of members and the path as an aggregate FILTER
-- shape: S6
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.mix,
	   count(*) FILTER (WHERE coldiff.doc_norm(w.doc) @? '$.parts[*] ? (@.tone == "down" && @.category == "service")') AS neg_service,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.category == "m_tidy" || @.category == "location")')) AS place,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.val == "wire")')) AS wire,
	   min(w.id) AS first_id
FROM t_work w
GROUP BY 1
ORDER BY 1 NULLS FIRST
