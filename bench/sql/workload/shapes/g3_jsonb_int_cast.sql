-- GAP 3: a jsonb -> integer cast in a filter ((ratings -> 'overall')::int = 9) keeps the normal plan; the text path
-- ((ratings ->> 'overall')::numeric::int = 9) is pushed.
\echo -- not pushed: jsonb cast
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_wl r WHERE (r.ratings -> 'overall')::int = 9;
\echo -- pushed: text path
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_wl r WHERE (r.ratings ->> 'overall')::numeric::int = 9;
