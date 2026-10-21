-- trap: != and ! over an astext leaf are unknown for a value of another type (the number 1 of a sign), which the text of the leaf cannot tell
-- shape: trap
-- expect: plain
-- key: -
SELECT count(*) FILTER (WHERE jsonb_path_exists(coldiff.load_norm(w.doc), '$.parts[*] ? (@.sign != "positive")')) AS ne,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.load_norm(w.doc), '$.parts[*] ? (!(@.sign == "positive"))')) AS not_eq
FROM t_load w
