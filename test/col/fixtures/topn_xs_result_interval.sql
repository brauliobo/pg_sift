-- an interval result is not a key the scan holds: refused
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, ts FROM t_coal WHERE ts < 'infinity' ORDER BY ts - TIMESTAMP '2024-02-01', id LIMIT 12
