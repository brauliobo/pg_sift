-- ORDER BY concat() of a number and a text: concat is stable, the scan runs PostgreSQL's own
-- shape: topn
-- expect: agg
-- key: -
SELECT id, t, grp FROM t_text ORDER BY concat(t, grp), id LIMIT 12 OFFSET 2
