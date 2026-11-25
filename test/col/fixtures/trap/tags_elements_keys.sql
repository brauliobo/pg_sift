-- the tag with a group of the row, and the tags of the rows that have the tag next to others (array test and unnest in one query)
-- shape: unnest
-- expect: agg
-- key: -
SELECT x.grp, t.tag, count(*) AS n
FROM t_tags x CROSS JOIN LATERAL jsonb_array_elements_text(x.tags) AS t(tag)
WHERE x.tags ? 'gift' OR x.grp = 1
GROUP BY 1, 2
ORDER BY 1, 2
