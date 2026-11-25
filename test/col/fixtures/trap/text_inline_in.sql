-- flat: IN and NOT IN lists of long text, NULL among the constants
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT (SELECT count(*) FROM t_text WHERE t IN (repeat('a', 250) || '0', repeat('b', 290) || '1', 'z', NULL)) AS in_null,
       (SELECT count(*) FROM t_text WHERE t NOT IN (repeat('a', 250) || '0', repeat('b', 290) || '1')) AS not_in,
       (SELECT count(*) FROM t_text WHERE t NOT IN (repeat('a', 250) || '0', NULL)) AS not_in_null
