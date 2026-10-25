-- a statement of 12 `LEFT JOIN LATERAL (aggregates) ON true` keyed by a dimension row, the first statement of a new backend: the flat query is planned first and, when the scan reads every indexed table of it, the query as written is not planned
-- label: 12 lateral sub-selects of `count`, `sum` over `events`, one for each row of a dimension, the first statement of a new backend
-- ordered: yes
-- fresh: yes
--@ pg
SELECT d.tier, sum(l1.n) AS n1, sum(l1.a) AS a1, sum(l2.n) AS n2, sum(l2.a) AS a2, sum(l3.n) AS n3, sum(l3.a) AS a3, sum(l4.n) AS n4, sum(l4.a) AS a4, sum(l5.n) AS n5, sum(l5.a) AS a5, sum(l6.n) AS n6, sum(l6.a) AS a6, sum(l7.n) AS n7, sum(l7.a) AS a7, sum(l8.n) AS n8, sum(l8.a) AS a8, sum(l9.n) AS n9, sum(l9.a) AS a9, sum(l10.n) AS n10, sum(l10.a) AS a10, sum(l11.n) AS n11, sum(l11.a) AS a11, sum(l12.n) AS n12, sum(l12.a) AS a12
FROM dims d
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_a' AND e.qty > 0) l1 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_b' AND e.qty > 1) l2 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_c' AND e.qty > 2) l3 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_d' AND e.qty > 3) l4 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_a' AND e.qty > 4) l5 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_b' AND e.qty > 0) l6 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_c' AND e.qty > 1) l7 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_d' AND e.qty > 2) l8 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_a' AND e.qty > 3) l9 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_b' AND e.qty > 4) l10 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_c' AND e.qty > 0) l11 ON true
LEFT JOIN LATERAL (SELECT count(*) AS n, sum(e.amount) AS a FROM events e WHERE e.parent_id = d.id AND e.deleted_at IS NULL AND e.kind = 'cat_d' AND e.qty > 1) l12 ON true
GROUP BY d.tier ORDER BY d.tier
