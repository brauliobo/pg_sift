-- the query is a parameter
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
-- prepare: (text)
-- args: 'size color'
SELECT source, count(*) AS n FROM orders WHERE memo @@> $1 GROUP BY source ORDER BY source
