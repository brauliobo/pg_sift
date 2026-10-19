-- a numeric column with NULLs: DESC puts them first unless told otherwise
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, total FROM orders WHERE memo @@> 'gift' ORDER BY total DESC NULLS LAST, id LIMIT 15
