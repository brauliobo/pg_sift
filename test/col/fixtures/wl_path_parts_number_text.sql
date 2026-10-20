-- trap: a string literal that a number or a boolean prints as ("1", "true"): the astext leaf holds the text of both, the jsonpath comparison is false for them
-- shape: trap
-- expect: plain
-- key: -
SELECT count(*) FILTER (WHERE jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.tone == "1")')) AS one,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.tone == "true")')) AS tru
FROM t_work w
