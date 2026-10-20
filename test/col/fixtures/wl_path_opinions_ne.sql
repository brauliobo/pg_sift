-- trap: != and ! over an astext leaf are unknown for a value of another type (the number 1 of a polarity), which the text of the leaf cannot tell
-- shape: trap
-- expect: plain
-- key: -
SELECT count(*) FILTER (WHERE jsonb_path_exists(coldiff.wl_norm(w.doc), '$.opinions[*] ? (@.polarity != "positive")')) AS ne,
	   count(*) FILTER (WHERE jsonb_path_exists(coldiff.wl_norm(w.doc), '$.opinions[*] ? (!(@.polarity == "positive"))')) AS not_eq
FROM t_wl w
