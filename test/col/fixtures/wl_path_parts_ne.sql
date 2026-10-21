-- trap: != and ! over an astext leaf are unknown for a value of another type (the number 1 of a sign), which the text of the leaf cannot tell
-- shape: trap
-- expect: plain
-- key: -
SELECT count(*) FILTER (WHERE jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.dir_a != "up")')) AS ne,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (!(@.dir_a == "up"))')) AS not_eq
FROM t_work w
