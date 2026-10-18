-- trap: a volatile expression inside the aggregate argument
-- shape: trap
-- expect: plain
-- key: -
SELECT source, sum(store_id + 0 * random()) AS s, count(*) AS n FROM orders GROUP BY source ORDER BY source
