-- the @? operator, && and || of members and the path as an aggregate FILTER
-- shape: S6
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.composition,
	   count(*) FILTER (WHERE coldiff.wl_norm(w.doc) @? '$.opinions[*] ? (@.polarity == "negative" && @.category == "service")') AS neg_service,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.wl_norm(w.doc), '$.opinions[*] ? (@.category == "cleanliness" || @.category == "location")')) AS place,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.wl_norm(w.doc), '$.opinions[*] ? (@.subrating == "wifi")')) AS wifi,
	   min(w.id) AS first_id
FROM t_wl w
GROUP BY 1
ORDER BY 1 NULLS FIRST
