-- query: title-match-by-kind
-- kind: bm25_then_join
-- join: inner
-- sift: SELECT g.kind, count(*) AS notes FROM note n JOIN grp g ON g.id = n.grp_id WHERE n.title @@> 'w1' GROUP BY g.kind
SELECT g.kind, count(*) AS notes FROM note n JOIN grp g ON g.id = n.grp_id WHERE to_tsvector('simple', n.title) @@ to_tsquery('simple', 'w1') GROUP BY g.kind;

-- query: body-two-words-by-tier
-- kind: bm25_then_join
-- join: inner
-- sift: SELECT g.tier, count(*) AS notes, count(DISTINCT n.grp_id) AS groups FROM note n JOIN grp g ON g.id = n.grp_id WHERE n.body @@> 'w2 w3' GROUP BY g.tier
SELECT g.tier, count(*) AS notes, count(DISTINCT n.grp_id) AS groups FROM note n JOIN grp g ON g.id = n.grp_id WHERE to_tsvector('simple', n.body) @@ to_tsquery('simple', 'w2 & w3') GROUP BY g.tier;

-- query: title-and-body-by-kind
-- kind: bm25_then_join
-- join: inner
-- sift: SELECT g.kind, count(*) AS notes FROM note n JOIN grp g ON g.id = n.grp_id WHERE n.title @@> 'w4' AND n.body @@> 'w5' AND g.tier >= 2 GROUP BY g.kind
SELECT g.kind, count(*) AS notes FROM note n JOIN grp g ON g.id = n.grp_id WHERE to_tsvector('simple', n.title) @@ to_tsquery('simple', 'w4') AND to_tsvector('simple', n.body) @@ to_tsquery('simple', 'w5') AND g.tier >= 2 GROUP BY g.kind;

-- query: top-groups-for-term
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
-- sift: SELECT g.id, g.kind, count(*) AS notes FROM note n JOIN grp g ON g.id = n.grp_id WHERE n.body @@> 'w1' GROUP BY g.id, g.kind ORDER BY notes DESC, g.id LIMIT 10
SELECT g.id, g.kind, count(*) AS notes FROM note n JOIN grp g ON g.id = n.grp_id WHERE to_tsvector('simple', n.body) @@ to_tsquery('simple', 'w1') GROUP BY g.id, g.kind ORDER BY notes DESC, g.id LIMIT 10;

-- query: groups-with-matching-notes
-- kind: exists_scope
-- join: semi
-- sift: SELECT g.kind, count(*) AS groups FROM grp g WHERE EXISTS (SELECT 1 FROM note n WHERE n.grp_id = g.id AND n.title @@> 'w7') GROUP BY g.kind
SELECT g.kind, count(*) AS groups FROM grp g WHERE EXISTS (SELECT 1 FROM note n WHERE n.grp_id = g.id AND to_tsvector('simple', n.title) @@ to_tsquery('simple', 'w7')) GROUP BY g.kind;

-- query: groups-without-matching-notes
-- kind: anti_join_count
-- join: anti
-- sift: SELECT count(*) AS groups FROM grp g WHERE NOT EXISTS (SELECT 1 FROM note n WHERE n.grp_id = g.id AND n.body @@> 'w1 w2')
SELECT count(*) AS groups FROM grp g WHERE NOT EXISTS (SELECT 1 FROM note n WHERE n.grp_id = g.id AND to_tsvector('simple', n.body) @@ to_tsquery('simple', 'w1 & w2'));

-- query: notes-by-kind
-- kind: group_by_dim_attr
-- join: inner
SELECT g.kind, count(*) AS notes, count(DISTINCT n.grp_id) AS groups FROM note n JOIN grp g ON g.id = n.grp_id GROUP BY g.kind;

-- query: term-mix-by-tier
-- kind: filter_agg_dim_attrs
-- join: inner
-- sift: SELECT g.tier, count(*) FILTER (WHERE g.kind = 'w1') AS kind1, count(*) AS notes FROM note n JOIN grp g ON g.id = n.grp_id WHERE n.body @@> 'w3' GROUP BY g.tier
SELECT g.tier, count(*) FILTER (WHERE g.kind = 'w1') AS kind1, count(*) AS notes FROM note n JOIN grp g ON g.id = n.grp_id WHERE to_tsvector('simple', n.body) @@ to_tsquery('simple', 'w3') GROUP BY g.tier;
