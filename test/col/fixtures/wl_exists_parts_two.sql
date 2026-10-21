-- two members of the same element, a member of another element does not count, a category list, and the EXISTS under OR and NOT
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.mix,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.load_norm(w.doc) -> 'parts') AS o(category text, polarity text) WHERE o.category = 'service' AND o.polarity = 'negative')) AS service_neg,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.load_norm(w.doc) -> 'parts') AS o(category text) WHERE o.category IN ('food', 'comfort'))) AS in_list,
	   count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.load_norm(w.doc) -> 'parts') AS o(val text) WHERE o.val = 'wifi')
					   OR EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.load_norm(w.doc) -> 'parts') AS o(val text) WHERE o.val = '')) AS or_not
FROM t_load w
WHERE w.deleted_at IS NULL
GROUP BY 1
ORDER BY 1 NULLS FIRST
