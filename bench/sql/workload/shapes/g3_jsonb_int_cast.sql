-- Status (3.2): follow-up: the jsonb cast raises for a value that is no number (docs/AGG.md, "Workload shapes")
-- GAP 3: a jsonb -> integer cast in a filter ((metrics -> 'm_total')::int = 9) keeps the normal plan; the text path
-- ((metrics ->> 'm_total')::numeric::int = 9) is pushed.
\echo -- not pushed: jsonb cast
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_work r WHERE (r.metrics -> 'm_total')::int = 9;
\echo -- pushed: text path
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_work r WHERE (r.metrics ->> 'm_total')::numeric::int = 9;
