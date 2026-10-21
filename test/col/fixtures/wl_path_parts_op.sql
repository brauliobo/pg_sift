-- the @? operator, && and || of members and the path as an aggregate FILTER
-- shape: S6
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.mix,
	   count(*) FILTER (WHERE coldiff.load_norm(w.doc) @? '$.parts[*] ? (@.polarity == "negative" && @.category == "service")') AS neg_service,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.load_norm(w.doc), '$.parts[*] ? (@.category == "cleanliness" || @.category == "location")')) AS place,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.load_norm(w.doc), '$.parts[*] ? (@.val == "wifi")')) AS wifi,
	   min(w.id) AS first_id
FROM t_load w
GROUP BY 1
ORDER BY 1 NULLS FIRST
