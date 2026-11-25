-- trap: DISTINCT ON of a column that the index does not hold
-- shape: trap
-- expect: plain
-- key: -
SELECT DISTINCT ON (left(memo, 3)) left(memo, 3) AS m, id FROM orders ORDER BY left(memo, 3), id
