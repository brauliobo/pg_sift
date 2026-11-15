-- query: top3-children-per-parent-by-group
-- kind: top_n_per_parent
-- join: inner
-- plan: SiftAgg
-- oracle: SELECT p.grp, count(*) AS children, sum(t.amount) AS amount FROM parent p JOIN (SELECT parent_id, amount, row_number() OVER (PARTITION BY parent_id ORDER BY amount DESC, id) AS rn FROM child) t ON t.parent_id = p.id AND t.rn <= 3 WHERE p.rank < 20 GROUP BY p.grp
SELECT p.grp, count(*) AS children, sum(t.amount) AS amount FROM parent p CROSS JOIN LATERAL (SELECT c.amount FROM child c WHERE c.parent_id = p.id ORDER BY c.amount DESC, c.id LIMIT 3) t WHERE p.rank < 20 GROUP BY p.grp;
-- query: parents-with-five-a-children-by-state
-- kind: parent_child_count
-- join: inner
-- plan: SiftAgg
-- oracle: SELECT p.state, count(*) AS parents FROM parent p JOIN (SELECT parent_id, count(*) AS n FROM child WHERE kind = 'a' GROUP BY parent_id) m ON m.parent_id = p.id AND m.n >= 5 GROUP BY p.state
SELECT p.state, count(*) AS parents FROM parent p WHERE (SELECT count(*) FROM child c WHERE c.parent_id = p.id AND c.kind = 'a') >= 5 GROUP BY p.state;
-- query: max-child-amount-per-parent-group
-- kind: child_agg_per_parent
-- join: inner
-- plan: SiftAgg
SELECT p.grp, max(m.top) AS top, count(*) AS parents FROM parent p JOIN (SELECT parent_id, max(amount) AS top FROM child GROUP BY parent_id) m ON m.parent_id = p.id GROUP BY p.grp;
-- query: kind-a-sum-per-parent-by-state
-- kind: child_agg_per_parent
-- join: inner
-- plan: SiftAgg
SELECT p.state, count(*) AS parents, sum(s.amount) AS amount FROM parent p JOIN (SELECT parent_id, sum(amount) AS amount FROM child WHERE kind = 'a' GROUP BY parent_id) s ON s.parent_id = p.id GROUP BY p.state;
-- query: children-per-parent-histogram
-- kind: child_count_per_parent
-- join: left
-- plan: SiftAgg
SELECT n, count(*) AS parents FROM (SELECT p.id, count(c.id) AS n FROM parent p LEFT JOIN child c ON c.parent_id = p.id GROUP BY p.id) t GROUP BY n;
-- query: parents-ranked-by-child-count
-- kind: top_n_across_join
-- join: inner
-- plan: SiftAgg
-- ordered: yes
SELECT p.id, count(*) AS children FROM parent p JOIN child c ON c.parent_id = p.id GROUP BY p.id ORDER BY children DESC, p.id LIMIT 10;
