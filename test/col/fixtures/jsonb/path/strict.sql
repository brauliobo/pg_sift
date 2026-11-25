-- jsonpath strict mode: the positive forms are the lax ones on these documents (a missing key is unknown, not false, and nothing here negates it)
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n,
       count(*) FILTER (WHERE jsonb_path_exists(doc, 'strict $[*] ? (@.n == 3)')) AS n_eq,
       count(*) FILTER (WHERE jsonb_path_exists(doc, 'strict $[*] ? (@.s != "gift" && @.b == true)')) AS s_ne_b,
       count(*) FILTER (WHERE jsonb_path_exists(doc, 'strict $[*] ? (@.n != 1 || @.s == "toy")')) AS either,
       count(*) FILTER (WHERE jsonb_path_exists(doc, 'lax $[*] ? (@.n != 1)')) AS lax_ne,
       count(*) FILTER (WHERE doc @? 'strict $[*] ? (@.s == "")') AS empty_s,
       sum(id) AS ids
FROM t_jp
GROUP BY grp
ORDER BY grp
