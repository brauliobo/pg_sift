-- prefix and wildcard terms
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT source, count(*) AS n FROM orders WHERE memo @@> 'exch*' OR memo @@> 'pick*' GROUP BY source ORDER BY source
