-- two members of the same element, a member of another element does not count, a category list, and the EXISTS under OR and NOT
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.mix,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(category text, dir_a text) WHERE o.category = 'catg_j' AND o.dir_a = 'down')) AS catg_j_neg,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(category text) WHERE o.category IN ('catg_d', 'catg_c'))) AS in_list,
	   count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(val text) WHERE o.val = 'catg_n')
					   OR EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(val text) WHERE o.val = '')) AS or_not
FROM t_work w
WHERE w.deleted_at IS NULL
GROUP BY 1
ORDER BY 1 NULLS FIRST
