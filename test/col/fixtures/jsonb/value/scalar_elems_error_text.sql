-- trap: a text that is no number in the member of an element, cast by the aggregate, raises the error of the cast for the row that reaches it
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- error: yes
-- key: -
SELECT count(*) AS n, sum((p ->> 'v')::float8) AS s
FROM t_vd, jsonb_array_elements(doc -> 'l') p WHERE grp = 2
