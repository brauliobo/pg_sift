-- trap: NOT of a path test over an index expression that is NULL for a document that is not an object: NOT NULL is NULL, the negation would count the row
-- shape: trap
-- expect: plain
-- key: -
SELECT w.mix, count(*) FILTER (WHERE NOT jsonb_path_exists(coldiff.doc_norm(w.doc), '$.parts[*] ? (@.val == "wire")')) AS no_wire
FROM t_work w
GROUP BY 1
ORDER BY 1 NULLS FIRST
