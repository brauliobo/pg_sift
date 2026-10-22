-- the text of an integer in the C collation
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY i4::text COLLATE "C" DESC, id LIMIT 12
