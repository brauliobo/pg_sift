-- jsonpath with constant variables: a string, a number, a boolean and a JSON null against the members, with != and !()
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.s == $v)', '{"v": "gift"}')) AS s_eq,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.s != $v)', '{"v": "gift"}')) AS s_ne,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n == $q)', '{"q": 3}')) AS n_eq,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n > $q)', '{"q": 1.0}')) AS n_gt,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.b == $t)', '{"t": true}')) AS b_eq,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.s == $z)', '{"z": null}')) AS s_null,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (!(@.n == $q) && @.s != $v)', '{"q": 3, "v": "toy", "unused": [1, 2]}')) AS mixed,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? ($q == @.n)', '{"q": 0}', true)) AS silent,
       sum(id) AS ids
FROM t_jp
GROUP BY grp
ORDER BY grp
