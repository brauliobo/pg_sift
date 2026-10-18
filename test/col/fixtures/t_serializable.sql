-- trap: SERIALIZABLE isolation (predicate locks on the table)
-- shape: trap
-- expect: any
-- isolation: serializable
-- key: -
SELECT source, count(*) AS n, avg(store_id) AS a FROM orders WHERE deleted_at IS NULL GROUP BY source ORDER BY source
