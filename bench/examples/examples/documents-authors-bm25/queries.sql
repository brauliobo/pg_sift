-- query: matching-documents-by-country
-- kind: bm25_then_join
-- join: inner
-- sift: SELECT a.country, count(*) AS documents FROM document d JOIN author a ON a.id = d.author_id WHERE d.body @@> 'w1 w2' GROUP BY a.country
SELECT a.country, count(*) AS documents FROM document d JOIN author a ON a.id = d.author_id WHERE to_tsvector('simple', d.body) @@ to_tsquery('simple', 'w1 & w2') GROUP BY a.country;

-- query: matching-documents-by-tier-recent
-- kind: bm25_then_join
-- join: inner
-- sift: SELECT a.tier, count(*) AS documents, count(DISTINCT d.author_id) AS authors FROM document d JOIN author a ON a.id = d.author_id WHERE d.body @@> 'w3 w4' AND d.published_at >= '2024-07-01' GROUP BY a.tier
SELECT a.tier, count(*) AS documents, count(DISTINCT d.author_id) AS authors FROM document d JOIN author a ON a.id = d.author_id WHERE to_tsvector('simple', d.body) @@ to_tsquery('simple', 'w3 & w4') AND d.published_at >= '2024-07-01' GROUP BY a.tier;

-- query: top-authors-for-term
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
-- sift: SELECT a.id, a.country, count(*) AS documents FROM document d JOIN author a ON a.id = d.author_id WHERE d.body @@> 'w1' GROUP BY a.id, a.country ORDER BY documents DESC, a.id LIMIT 10
SELECT a.id, a.country, count(*) AS documents FROM document d JOIN author a ON a.id = d.author_id WHERE to_tsvector('simple', d.body) @@ to_tsquery('simple', 'w1') GROUP BY a.id, a.country ORDER BY documents DESC, a.id LIMIT 10;

-- query: documents-by-country
-- kind: group_by_dim_attr
-- join: inner
SELECT a.country, count(*) AS documents, count(DISTINCT d.author_id) AS authors FROM document d JOIN author a ON a.id = d.author_id GROUP BY a.country;

-- query: filter-author-then-term
-- kind: filter_dim_attr
-- join: inner
-- sift: SELECT count(*) AS documents FROM document d JOIN author a ON a.id = d.author_id WHERE a.tier = 4 AND a.country = 'c1' AND d.body @@> 'w5'
SELECT count(*) AS documents FROM document d JOIN author a ON a.id = d.author_id WHERE a.tier = 4 AND a.country = 'c1' AND to_tsvector('simple', d.body) @@ to_tsquery('simple', 'w5');

-- query: authors-without-matching-documents
-- kind: anti_join_count
-- join: anti
-- sift: SELECT count(*) AS authors FROM author a WHERE NOT EXISTS (SELECT 1 FROM document d WHERE d.author_id = a.id AND d.body @@> 'w1 w2 w3')
SELECT count(*) AS authors FROM author a WHERE NOT EXISTS (SELECT 1 FROM document d WHERE d.author_id = a.id AND to_tsvector('simple', d.body) @@ to_tsquery('simple', 'w1 & w2 & w3'));

-- query: term-mix-by-tier
-- kind: filter_agg_dim_attrs
-- join: inner
-- sift: SELECT a.tier, count(*) FILTER (WHERE a.country = 'c1') AS country1, count(*) AS documents FROM document d JOIN author a ON a.id = d.author_id WHERE d.body @@> 'w2' GROUP BY a.tier
SELECT a.tier, count(*) FILTER (WHERE a.country = 'c1') AS country1, count(*) AS documents FROM document d JOIN author a ON a.id = d.author_id WHERE to_tsvector('simple', d.body) @@ to_tsquery('simple', 'w2') GROUP BY a.tier;
