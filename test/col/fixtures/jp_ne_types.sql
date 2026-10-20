-- jsonpath != on every member type: a JSON null differs from every literal, a missing key from none, equal numbers (1 and 1.0) and the empty string compare as values
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.s != "gift")')) AS s_gift,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.s != "")')) AS s_empty,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.s != "ünï")')) AS s_unicode,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n != 1)')) AS n_one,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n != 1.0)')) AS n_one_float,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n != -2)')) AS n_neg,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n != 2147483647)')) AS n_max,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (3 != @.n)')) AS n_commuted,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.b != true)')) AS b_true,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.b != false)')) AS b_false,
       count(*) FILTER (WHERE doc @? '$[*] ? (@.s != "toy" && @.n != 0)') AS both,
       count(*) FILTER (WHERE doc @? '$[*] ? (@.s != "toy" || @.b != true)') AS either,
       count(*) FILTER (WHERE jsonb_path_exists(mdoc, '$[*] ? (@.s != "gift")')) AS nullable_doc,
       count(*) FILTER (WHERE NOT jsonb_path_exists(doc, '$[*] ? (@.s != "gift")')) AS no_s_gift,
       sum(id) AS ids
FROM t_jp
GROUP BY grp
ORDER BY grp
