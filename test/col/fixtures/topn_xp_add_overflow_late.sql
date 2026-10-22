-- a late conjunct (a column outside the index's leaves of the scan) filters the rows that would overflow
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 < 2147483647 AND grp <> 0 ORDER BY i4 + 1 DESC, id LIMIT 12
