-- the polarity that is a JSON number, a boolean or null in some opinions: the recordset reads them as text, so does the astext leaf
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT w.segment, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.wl_norm(w.doc) -> 'opinions') AS o(polarity text) WHERE o.polarity = '1')) AS one,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.wl_norm(w.doc) -> 'opinions') AS o(polarity text) WHERE o.polarity = 'true')) AS tru,
	   count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.wl_norm(w.doc) -> 'opinions') AS o(polarity text) WHERE o.polarity IS NULL)) AS nul,
	   count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(coldiff.wl_norm(w.doc) -> 'opinions') AS o(polarity text) WHERE o.polarity <> 'positive')) AS only_positive
FROM t_wl w
GROUP BY 1
ORDER BY 1 NULLS FIRST
