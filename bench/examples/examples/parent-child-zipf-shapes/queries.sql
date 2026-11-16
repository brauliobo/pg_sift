-- query: parents-with-big-child-by-group
-- kind: parent_exists_child
-- join: semi
SELECT p.grp, count(*) AS parents FROM parent p WHERE EXISTS (SELECT 1 FROM child c WHERE c.parent_id = p.id AND c.amount > 990) GROUP BY p.grp;
-- query: parents-without-big-d-child-by-state
-- kind: parent_not_exists_child
-- join: anti
-- plan: SiftAgg
SELECT p.state, count(*) AS parents, sum(p.rank) AS ranks FROM parent p WHERE NOT EXISTS (SELECT 1 FROM child c WHERE c.parent_id = p.id AND c.kind = 'd' AND c.amount > 990) GROUP BY p.state;
-- query: parents-in-recent-a-children
-- kind: parent_exists_child
-- join: semi
SELECT p.state, count(*) AS parents, sum(p.rank) AS ranks FROM parent p WHERE p.id IN (SELECT parent_id FROM child WHERE kind = 'a' AND at >= timestamp '2024-06-01') AND p.rank < 50 GROUP BY p.state;
-- query: child-count-per-parent-group
-- kind: child_count_per_parent
-- join: left
-- plan: SiftAgg
SELECT p.grp, count(c.id) AS children, count(DISTINCT p.id) AS parents FROM parent p LEFT JOIN child c ON c.parent_id = p.id GROUP BY p.grp;
-- query: left-join-sum-distinct-by-state
-- kind: parent_left_agg
-- join: left
-- plan: SiftAgg
SELECT p.state, count(*) AS joined_rows, sum(c.amount) AS amount, count(DISTINCT c.kind) AS kinds FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.amount > 500 GROUP BY p.state;
-- query: parents-never-d-left-anti
-- kind: parent_not_exists_child
-- join: anti
SELECT p.grp, count(*) AS parents FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.kind = 'd' WHERE c.id IS NULL GROUP BY p.grp;
-- query: latest-child-by-group
-- kind: latest_per_group
-- join: inner
-- plan: SiftAgg
-- oracle: SELECT p.grp, count(*) AS parents, sum(c.amount) AS amount FROM parent p JOIN child c ON c.parent_id = p.id JOIN (SELECT parent_id, max(at) AS at FROM child GROUP BY parent_id) m ON m.parent_id = c.parent_id AND m.at = c.at GROUP BY p.grp
SELECT p.grp, count(*) AS parents, sum(c.amount) AS amount FROM parent p JOIN child c ON c.parent_id = p.id
WHERE c.at = (SELECT max(x.at) FROM child x WHERE x.parent_id = p.id) GROUP BY p.grp;
-- query: top-parents-by-child-sum
-- kind: top_n_across_join
-- join: inner
-- plan: SiftAgg
-- ordered: yes
SELECT p.id, count(*) AS children, sum(c.amount) AS amount FROM parent p JOIN child c ON c.parent_id = p.id GROUP BY p.id ORDER BY amount DESC, p.id LIMIT 10;
