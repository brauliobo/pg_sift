-- query: group-by-parent-region
-- kind: group_by_dim_attr
-- join: inner
-- plan: SiftAgg
SELECT p.region AS v, count(*) AS children, sum(c.amount) AS amount FROM child c JOIN parent p ON p.id = c.parent_id GROUP BY p.region;
-- query: filter-parent-status
-- kind: filter_dim_attr
-- join: inner
-- plan: SiftAgg
SELECT c.status, count(*) AS children, count(DISTINCT c.parent_id) AS parents FROM child c JOIN parent p ON p.id = c.parent_id WHERE p.region IN ('w2', 'w3', 'w7') GROUP BY c.status;
-- query: distinct-parent-by-status
-- kind: count_distinct_across_join
-- join: inner
-- plan: SiftAgg
SELECT c.status, count(DISTINCT p.region) AS variants FROM child c JOIN parent p ON p.id = c.parent_id GROUP BY c.status;
-- query: parent-filter-with-other-column
-- kind: filter_agg_dim_attrs
-- join: inner
-- plan: SiftAgg
SELECT p.region AS v, count(*) AS children, count(*) FILTER (WHERE p.name = 'n1') AS n1 FROM child c JOIN parent p ON p.id = c.parent_id WHERE p.region IN ('w2', 'w3', 'w7') GROUP BY p.region;
-- query: left-from-child-group
-- kind: group_by_dim_attr
-- join: left
-- plan: SiftAgg
SELECT p.region AS v, count(*) AS children FROM child c LEFT JOIN parent p ON p.id = c.parent_id GROUP BY p.region;
-- query: semi-parent-filter
-- kind: filter_dim_attr
-- join: semi
-- plan: SiftAgg
SELECT count(*) AS children, sum(c.amount) AS amount FROM child c WHERE c.parent_id IN (SELECT id FROM parent p WHERE p.region IN ('w2', 'w3', 'w7'));
