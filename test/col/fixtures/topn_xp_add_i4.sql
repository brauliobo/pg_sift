-- ORDER BY int + constant: the order of the column, the integer overflow of the sum is raised for the rows the WHERE keeps (none of them here)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 < 1000 ORDER BY i4 + 1, id LIMIT 12
