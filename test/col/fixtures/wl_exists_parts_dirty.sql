-- the tone that is a JSON number, a boolean or null in some parts: the recordset reads them as text, so does the astext leaf
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.segment, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(tone text) WHERE o.tone = '1')) AS one,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(tone text) WHERE o.tone = 'true')) AS tru,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(tone text) WHERE o.tone IS NULL)) AS nul,
	   count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.doc_norm(w.doc) -> 'parts') AS o(tone text) WHERE o.tone <> 'up')) AS only_positive
FROM t_work w
GROUP BY 1
ORDER BY 1 NULLS FIRST
