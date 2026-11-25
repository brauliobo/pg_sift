-- trap: a literal of another JSON type than the member leaf's is unknown for every value of the leaf, true only for a JSON null
-- shape: trap
-- expect: plain
-- key: -
SELECT grp, count(*) AS n,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n != "3")')) AS number_vs_string,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.s != 3)')) AS string_vs_number
FROM t_jp
GROUP BY grp
ORDER BY grp
