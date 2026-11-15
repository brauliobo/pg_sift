-- query: children-and-distinct-parents-by-source
-- kind: count_distinct_across_join
-- join: inner
SELECT s.id, count(*) AS children, count(DISTINCT c.parent_id) AS parents
FROM child c JOIN parent p ON p.id = c.parent_id JOIN source s ON s.id = p.feed_id GROUP BY s.id;

-- query: scoped-children-by-city
-- kind: exists_scope
-- join: semi
SELECT p.city, count(*) AS children FROM child c JOIN parent p ON p.id = c.parent_id
WHERE EXISTS (SELECT 1 FROM assignment a WHERE a.feed_id = p.feed_id AND a.user_id = 1) GROUP BY p.city;

-- query: scoped-distinct-parents-by-source-kind
-- kind: chain
-- join: semi
SELECT s.kind, count(DISTINCT c.parent_id) AS parents, count(*) AS children
FROM child c JOIN parent p ON p.id = c.parent_id JOIN source s ON s.id = p.feed_id
WHERE EXISTS (SELECT 1 FROM assignment a WHERE a.feed_id = s.id AND a.user_id = 2) GROUP BY s.kind;

-- query: filters-over-parent-attributes
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT s.kind, count(*) FILTER (WHERE p.bucket_a >= 4) AS high, count(*) FILTER (WHERE p.flag_a) AS flagged,
       count(DISTINCT c.parent_id) FILTER (WHERE p.city = 'c1') AS city1
FROM child c JOIN parent p ON p.id = c.parent_id JOIN source s ON s.id = p.feed_id GROUP BY s.kind;

-- query: missing-field-counts-by-source
-- kind: missing_field_count
-- join: inner
SELECT s.id, count(*) FILTER (WHERE p.label IS NULL OR p.label = '') AS blank_label, count(*) FILTER (WHERE p.city IS NULL) AS no_city,
       count(*) FILTER (WHERE c.body IS NULL OR c.body = '') AS blank_body
FROM child c JOIN parent p ON p.id = c.parent_id JOIN source s ON s.id = p.feed_id GROUP BY s.id;

-- query: composite-source-parent-join
-- kind: group_by_dim_attr
-- join: inner
SELECT p.city, count(*) AS children FROM child c JOIN parent p ON p.feed_id = c.feed_id AND p.id = c.parent_id GROUP BY p.city;

-- query: scoped-parents-without-children
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS childless FROM parent p WHERE EXISTS (SELECT 1 FROM assignment a WHERE a.feed_id = p.feed_id AND a.user_id = 1)
AND NOT EXISTS (SELECT 1 FROM child c WHERE c.parent_id = p.id);

-- query: top-parents-by-children
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT p.id, p.city, count(*) AS children FROM child c JOIN parent p ON p.id = c.parent_id GROUP BY p.id, p.city ORDER BY children DESC, p.id LIMIT 10;

-- query: recent-children-in-region
-- kind: filter_dim_attr
-- join: inner
SELECT s.kind, count(*) AS children, sum(c.score) AS score FROM child c JOIN parent p ON p.id = c.parent_id JOIN source s ON s.id = p.feed_id
WHERE c.created_at >= '2024-09-01' AND s.region = 'r2' GROUP BY s.kind;

-- query: duplicate-pairs
-- kind: self_join
-- join: inner
SELECT count(*) AS duplicate_pairs FROM parent a JOIN parent b ON a.feed_id = b.feed_id AND a.external_key = b.external_key AND a.id < b.id;

-- query: sources-by-kind-right
-- kind: group_by_dim_attr
-- join: right
SELECT s.kind, count(p.id) AS parents, count(*) AS rows FROM parent p RIGHT JOIN source s ON s.id = p.feed_id GROUP BY s.kind;

-- query: parent-source-match-counts-full
-- kind: anti_join_count
-- join: full
SELECT count(*) FILTER (WHERE p.id IS NULL) AS sources_only, count(*) FILTER (WHERE s.id IS NULL) AS parents_only, count(*) AS rows FROM parent p FULL JOIN source s ON s.id = p.feed_id;

-- query: users-assigned-to-both-sources
-- kind: set_algebra
-- join: inner
SELECT count(*) AS users FROM (SELECT a.user_id FROM assignment a WHERE a.feed_id IN (1, 2) GROUP BY a.user_id HAVING count(DISTINCT a.feed_id) = 2) x;
