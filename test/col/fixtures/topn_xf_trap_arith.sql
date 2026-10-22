-- arithmetic can raise in a row that is not in the top rows (integer out of range: the normal plan evaluates it for all of them): refused
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 < 1000 ORDER BY i4 + 1, id LIMIT 12
