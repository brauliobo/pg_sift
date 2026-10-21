-- an aggregate over an OR of searches is not the top-N: the scan of the aggregate keeps its plan
-- shape: topn_or
-- expect: any
-- key: -
-- scenario: yes
SELECT source, count(*) AS n FROM orders WHERE memo @@> 'cable' OR memo @@> 'wire' GROUP BY source ORDER BY source
