-- trap: NOT of a $.key[*] path test: doc -> 'key' is NULL for a document without the key, where the call is false and its negation true, and for a NULL document, where both are NULL;
-- the elements cannot tell the two apart, so the negated test keeps the normal plan
-- shape: trap
-- expect: plain
-- key: -
SELECT w.mix, count(*) FILTER (WHERE NOT jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.val == "catg_n")')) AS no_catg_n
FROM t_work w
GROUP BY 1
ORDER BY 1 NULLS FIRST
