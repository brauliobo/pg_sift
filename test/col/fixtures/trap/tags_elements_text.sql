-- jsonb_array_elements_text over an array of strings: empty arrays, a tag repeated in one array, a filter on the tag, a count of distinct rows
-- shape: unnest
-- expect: agg
-- key: -
SELECT t.tag, count(*) AS n, count(DISTINCT x.id) AS tagged_rows, count(DISTINCT x.grp) AS groups
FROM t_tags x CROSS JOIN LATERAL jsonb_array_elements_text(x.tags) AS t(tag)
WHERE x.grp <> 3 AND t.tag <> 'rare'
GROUP BY t.tag
ORDER BY t.tag
