-- an integer cast to text sorts as text
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY i4::text, id LIMIT 12
