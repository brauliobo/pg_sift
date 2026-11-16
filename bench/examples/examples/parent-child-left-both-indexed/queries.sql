-- query: left-join-count-by-group-and-kind
-- kind: parent_left_agg
-- join: left
SELECT p.grp, c.kind, count(*) AS joined_rows, count(c.id) AS children FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.at >= timestamp '2024-07-01' GROUP BY p.grp, c.kind;
-- query: left-join-both-sides-filtered
-- kind: parent_left_agg
-- join: left
SELECT p.state, count(c.id) AS children, count(DISTINCT p.id) AS parents, sum(c.amount) AS amount FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.kind = 'b' WHERE p.rank >= 50 GROUP BY p.state;
-- query: parents-without-recent-children
-- kind: parent_not_exists_child
-- join: anti
SELECT p.grp, count(*) AS parents FROM parent p LEFT JOIN child c ON c.parent_id = p.id AND c.at >= timestamp '2024-12-01' WHERE c.id IS NULL GROUP BY p.grp;
-- query: right-join-children-per-group
-- kind: parent_left_agg
-- join: right
SELECT p.grp, count(c.id) AS children, count(*) AS joined_rows FROM child c RIGHT JOIN parent p ON c.parent_id = p.id GROUP BY p.grp;
-- query: full-join-orphans-and-childless
-- kind: parent_left_agg
-- join: full
SELECT (p.id IS NULL) AS orphan_child, (c.id IS NULL) AS childless_parent, count(*) AS joined_rows FROM parent p FULL JOIN child c ON c.parent_id = p.id GROUP BY 1, 2;
-- query: child-sum-by-parent-state-and-kind
-- kind: child_agg_per_parent
-- join: inner
-- plan: SiftAgg
SELECT p.state, c.kind, sum(c.amount) AS amount, count(*) AS children FROM parent p JOIN child c ON c.parent_id = p.id WHERE p.rank < 50 GROUP BY p.state, c.kind;
