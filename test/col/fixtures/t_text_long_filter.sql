-- flat: an equality, an IN list and a prefix on text longer than a dictionary entry
-- shape: flat
-- expect: any
-- key: -
-- scenario: yes
SELECT count(*) AS eq, count(*) FILTER (WHERE t LIKE 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb%' COLLATE "C") AS pre,
       count(*) FILTER (WHERE t IN (repeat('a', 250) || '0', repeat('b', 290) || '1', 'z')) AS inl
FROM t_text WHERE t = repeat('c', 330) || '2' OR t LIKE 'b%' OR t LIKE 'a%'
