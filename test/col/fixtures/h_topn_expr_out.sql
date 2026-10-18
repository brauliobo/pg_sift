-- the output is computed from the heap row of the winners
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, upper(memo) AS m, coalesce(total, 0) * 2 AS t2 FROM orders WHERE memo @@> 'perfect' ORDER BY id DESC LIMIT 6
