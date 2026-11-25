-- trap: strict mode makes a missing key unknown, so its !() is not true where lax mode's is
-- shape: trap
-- expect: plain
-- key: -
SELECT grp, count(*) AS n,
       count(*) FILTER (WHERE jsonb_path_exists(doc, 'strict $[*] ? (!(@.s == "gift"))')) AS strict_not,
       count(*) FILTER (WHERE jsonb_path_exists(doc, 'strict $[*] ? (!(@.n != 3))')) AS strict_not_ne
FROM t_jp
GROUP BY grp
ORDER BY grp
