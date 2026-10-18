-- having and order by an aggregate on a match set
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT store_id, count(*) AS n FROM orders WHERE memo @@> 'late OR missing' GROUP BY store_id HAVING count(*) > 2 ORDER BY n DESC, store_id LIMIT 20
