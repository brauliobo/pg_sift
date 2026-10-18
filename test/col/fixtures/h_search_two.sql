-- two BM25 conjuncts and a column filter on the same index
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT source, count(*) AS n FROM orders WHERE memo @@> 'fast' AND memo @@> 'gift OR express' AND total > 20 GROUP BY source ORDER BY source
