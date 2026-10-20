-- jsonpath !(): true for a missing key and a JSON null under ==, for a missing key only under !=, De Morgan over && and ||, double negation
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.s == "gift"))')) AS not_eq,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.s != "gift"))')) AS not_ne,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.n > 1))')) AS not_gt,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.n <= 0))')) AS not_le,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.n == 1 || @.b == true))')) AS not_or,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.s == "toy" && @.n < 0))')) AS not_and,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(!(@.s == "gift")))')) AS not_not,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.s == null))')) AS not_null,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.b != null))')) AS not_not_null,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.s != "toy") && @.n != 3)')) AS mixed,
       count(*) FILTER (WHERE NOT jsonb_path_exists(doc, '$[*] ? (!(@.s == "gift"))')) AS row_not,
       sum(id) AS ids
FROM t_jp
GROUP BY grp
ORDER BY grp
