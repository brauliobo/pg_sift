-- a column aggregate joined to a ranked scan: an inner join to the sub-select that carries the score of its top 50 (the ties broken by id)
-- shape: search
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS revenue, max(r.score) AS best
FROM orders o JOIN (SELECT id, sift.score(ctid) AS score FROM orders WHERE memo @@> 'cable wire' ORDER BY sift.score(ctid) DESC, id LIMIT 50) r ON o.id = r.id
GROUP BY o.status ORDER BY o.status NULLS FIRST
