-- trap: != and ! over an astext leaf are unknown for a value of another type (the number 1 of a polarity), which the text of the leaf cannot tell
-- shape: trap
-- expect: plain
-- key: -
SELECT count(*) FILTER (WHERE jsonb_path_exists(coldiff.load_norm(w.doc), '$.parts[*] ? (@.polarity != "positive")')) AS ne,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.load_norm(w.doc), '$.parts[*] ? (!(@.polarity == "positive"))')) AS not_eq
FROM t_load w
