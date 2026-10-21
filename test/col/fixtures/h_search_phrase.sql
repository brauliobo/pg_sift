-- a phrase: the rows the index leaves undecided are rechecked on their text
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT lang, count(*) AS n, sum(tenant_id) AS s FROM orders WHERE memo @@> '"cable wire"' GROUP BY lang ORDER BY lang
