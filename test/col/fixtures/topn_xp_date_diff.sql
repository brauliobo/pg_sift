-- date minus date
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, d FROM t_coal WHERE d < DATE '2030-01-01' AND d > DATE '2000-01-01' ORDER BY d - DATE '2024-02-01' DESC, id LIMIT 12
