-- jsonpath == and != against null, and containment of a JSON null: only a JSON null equals null, a missing key equals nothing
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.s == null)')) AS s_null,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n != null)')) AS n_not_null,
       count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.b == null)')) AS b_null,
       count(*) FILTER (WHERE doc @> '[{"s": null}]') AS contains_s,
       count(*) FILTER (WHERE doc @> '[{"n": null, "b": true}]') AS contains_n_b,
       count(*) FILTER (WHERE doc @> '[{"s": "gift"}, {"b": null}]') AS contains_two,
       count(*) FILTER (WHERE NOT doc @> '[{"b": null}]') AS no_b_null,
       sum(id) AS ids
FROM t_jp
GROUP BY grp
ORDER BY grp
