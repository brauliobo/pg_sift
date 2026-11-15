-- query: revenue-by-root
-- kind: self_join
-- join: inner
SELECT r.name, count(*) AS n, sum(l.price) AS price FROM listing l JOIN category c ON c.id = l.category_id JOIN category b ON b.id = c.parent_id JOIN category r ON r.id = b.parent_id GROUP BY r.name;

-- query: live-listings-by-branch
-- kind: filter_dim_attr
-- join: inner
SELECT b.name, count(*) AS n FROM listing l JOIN category c ON c.id = l.category_id JOIN category b ON b.id = c.parent_id WHERE l.status = 'live' AND b.name IN ('branch1', 'branch2') GROUP BY b.name;

-- query: distinct-leaves-by-root
-- kind: count_distinct_across_join
-- join: inner
SELECT r.name, count(DISTINCT l.category_id) AS leaves, count(DISTINCT b.id) AS branches FROM listing l JOIN category c ON c.id = l.category_id JOIN category b ON b.id = c.parent_id JOIN category r ON r.id = b.parent_id GROUP BY r.name;

-- query: leaves-without-listings
-- kind: anti_join_count
-- join: anti
SELECT b.name, count(*) AS empty_leaves FROM category c JOIN category b ON b.id = c.parent_id WHERE c.depth = 2 AND NOT EXISTS (SELECT 1 FROM listing l WHERE l.category_id = c.id) GROUP BY b.name;

-- query: status-by-root-filter
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT r.name, sum(l.price) FILTER (WHERE l.status = 'sold') AS sold, sum(l.price) FILTER (WHERE c.name = 'leaf1') AS leaf1, count(*) AS n
FROM listing l JOIN category c ON c.id = l.category_id JOIN category b ON b.id = c.parent_id JOIN category r ON r.id = b.parent_id GROUP BY r.name;

-- query: top-branches-by-price
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT b.id, b.name, sum(l.price) AS price FROM listing l JOIN category c ON c.id = l.category_id JOIN category b ON b.id = c.parent_id GROUP BY b.id, b.name ORDER BY price DESC, b.id LIMIT 10;

-- query: children-per-parent
-- kind: self_join
-- join: inner
SELECT p.depth, count(*) AS children, count(DISTINCT p.id) AS parents FROM category c JOIN category p ON p.id = c.parent_id GROUP BY p.depth;

-- query: listings-under-root-semi
-- kind: exists_scope
-- join: semi
SELECT count(*) AS n, sum(l.price) AS price FROM listing l WHERE EXISTS
 (SELECT 1 FROM category c JOIN category b ON b.id = c.parent_id JOIN category r ON r.id = b.parent_id WHERE c.id = l.category_id AND r.name = 'root1');
