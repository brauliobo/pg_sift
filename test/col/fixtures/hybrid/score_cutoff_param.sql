-- the cutoff and the query are parameters of a prepared statement (a generic plan)
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
-- prepare: (text, float4, int)
-- args: 'damaged OR broken OR refund', 2.5, 30
SELECT id, round(sift.score(ctid)::numeric, 3) AS score FROM orders
WHERE memo @@> $1 AND sift.score(ctid) > $2 ORDER BY sift.score(ctid) DESC, id LIMIT $3
