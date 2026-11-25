-- a query no document matches
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT source, count(*) AS n FROM orders WHERE memo @@> 'zzzabsent' GROUP BY source ORDER BY source
