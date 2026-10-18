-- a fuzzy term
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, max(id) AS last_id FROM orders WHERE memo @@> 'dammaged~1' GROUP BY status ORDER BY status
