-- DISTINCT ON over text and varchar columns, a sort key of the C collation next to one of the column's
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (grp) grp, id, t, vc FROM t_text ORDER BY grp, t COLLATE "C" DESC NULLS LAST, vc, id
