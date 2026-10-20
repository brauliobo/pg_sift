-- two members of the same element, a member of another element does not count, a category list, and the EXISTS under OR and NOT
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.label_a,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(category text, tone text) WHERE o.category = 'service' AND o.tone = 'down')) AS service_neg,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(category text) WHERE o.category IN ('food', 'comfort'))) AS in_list,
	   count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(value_num text) WHERE o.value_num = 'wire')
					   OR EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(value_num text) WHERE o.value_num = '')) AS or_not
FROM t_work w
WHERE w.deleted_at IS NULL
GROUP BY 1
ORDER BY 1 NULLS FIRST
