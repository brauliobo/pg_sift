-- trap: the transaction columns are the table's own: an aggregate that reads xmin keeps the plain plan
-- shape: trap
-- expect: plain
-- scenario: yes
-- key: -
SELECT status, count(*) AS n, count(xmin::text) AS with_xmin, count(DISTINCT xmin::text) AS xmins FROM orders GROUP BY status ORDER BY status
