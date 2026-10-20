-- trap: NOT of a path test over an index expression that is NULL for a document that is not an object: NOT NULL is NULL, the negation would count the row
-- shape: trap
-- expect: plain
-- key: -
SELECT w.composition, count(*) FILTER (WHERE NOT jsonb_path_exists(coldiff.wl_norm(w.doc), '$.opinions[*] ? (@.subrating == "wifi")')) AS no_wifi
FROM t_wl w
GROUP BY 1
ORDER BY 1 NULLS FIRST
